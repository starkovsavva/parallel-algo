#include <atomic>
#include <memory>
#include <thread>
#include <functional>
#include <vector>
#include <cassert>
#include <iostream>

// Простая, корректная реализация hazard pointers

static const unsigned MAX_HAZARD_POINTERS = 100;

struct HazardPointer {
    std::atomic<std::thread::id> id;
    std::atomic<void*> pointer;
    HazardPointer() : id(std::thread::id()), pointer(nullptr) {}
};

static HazardPointer hazard_pointers[MAX_HAZARD_POINTERS];

class HazardPointerOwner {
    HazardPointer* hp;
public:
    HazardPointerOwner(const HazardPointerOwner&) = delete;
    HazardPointerOwner& operator=(const HazardPointerOwner&) = delete;

    HazardPointerOwner() noexcept : hp(nullptr) {
        std::thread::id this_id = std::this_thread::get_id();
        for (unsigned i = 0; i < MAX_HAZARD_POINTERS; ++i) {
            std::thread::id empty_id;
            if (hazard_pointers[i].id.compare_exchange_strong(
                    empty_id, this_id,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                hp = &hazard_pointers[i];
                break;
            }
        }
        if (!hp) {
            // In a production system you'd want a dynamic pool or fallback.
            throw std::runtime_error("No hazard pointers available");
        }
    }

    std::atomic<void*>& get_pointer() {
        assert(hp);
        return hp->pointer;
    }

    void set_ptr(void* p) {
        get_pointer().store(p, std::memory_order_release);
    }

    void clear() {
        get_pointer().store(nullptr, std::memory_order_release);
    }

    ~HazardPointerOwner() {
        if (hp) {
            clear();
            hp->id.store(std::thread::id(), std::memory_order_release);
        }
    }
};

std::vector<void*> collect_active_hazard_pointers() {
    std::vector<void*> res;
    res.reserve(MAX_HAZARD_POINTERS);
    for (unsigned i = 0; i < MAX_HAZARD_POINTERS; ++i) {
        void* p = hazard_pointers[i].pointer.load(std::memory_order_acquire);
        if (p) res.push_back(p);
    }
    return res;
}

// Структуры для отложенного удаления
struct DataToReclaim {
    void* data;
    std::function<void(void*)> deleter;
    DataToReclaim* next;
    DataToReclaim(void* p, std::function<void(void*)> d) : data(p), deleter(d), next(nullptr) {}
};

static std::atomic<DataToReclaim*> nodes_to_reclaim{nullptr};

void add_to_reclaim_list(DataToReclaim* node) {
    node->next = nodes_to_reclaim.load(std::memory_order_acquire);
    while (!nodes_to_reclaim.compare_exchange_weak(
        node->next, node, std::memory_order_release, std::memory_order_relaxed)) {}
}

void delete_nodes_with_no_hazards() {
    DataToReclaim* current = nodes_to_reclaim.exchange(nullptr, std::memory_order_acq_rel);
    if (!current) return;

    std::vector<void*> hazards = collect_active_hazard_pointers();

    DataToReclaim* remaining = nullptr;

    while (current) {
        DataToReclaim* next = current->next;
        bool hazardous = false;
        for (void* hp : hazards) {
            if (hp == current->data) { hazardous = true; break; }
        }
        if (hazardous) {
            current->next = remaining;
            remaining = current;
        } else {
            current->deleter(current->data);
            delete current;
        }
        current = next;
    }

    if (remaining) {
        DataToReclaim* tail = remaining;
        while (tail->next) tail = tail->next;
        tail->next = nodes_to_reclaim.load(std::memory_order_acquire);
        while (!nodes_to_reclaim.compare_exchange_weak(
            tail->next, remaining, std::memory_order_release, std::memory_order_relaxed)) {}
    }
}

// thread-local retired list with threshold to move into global list
template<typename T>
void reclaim_later(T* data) {
    // per-thread collection
    thread_local std::vector<DataToReclaim*> retired;
    const unsigned RECLAIM_THRESHOLD = 100;

    retired.push_back(new DataToReclaim(data, [](void* p){ delete static_cast<T*>(p); }));
    if (retired.size() >= RECLAIM_THRESHOLD) {
        // move them to global list
        for (DataToReclaim* node : retired) add_to_reclaim_list(node);
        retired.clear();
        delete_nodes_with_no_hazards();
    }
}

// Узел очереди
template<typename T>
struct Node {
    std::shared_ptr<T> data;
    std::atomic<Node<T>*> next;
    Node() : data(nullptr), next(nullptr) {}
    explicit Node(T&& value) : data(std::make_shared<T>(std::move(value))), next(nullptr) {}
};

template<typename T>
class LockFreeQueue {
private:
    alignas(64) std::atomic<Node<T>*> head_;
    alignas(64) std::atomic<Node<T>*> tail_;

public:
    LockFreeQueue() {
        Node<T>* dummy = new Node<T>();
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }

    ~LockFreeQueue() {
        // извлекаем все элементы
        while (pop() != nullptr) {}
        Node<T>* last = head_.load(std::memory_order_relaxed);
        delete last;
        // финальная попытка освобождения
        delete_nodes_with_no_hazards();
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    void push(T value) {
        Node<T>* new_node = new Node<T>(std::move(value));
        while (true) {
            Node<T>* current_tail = tail_.load(std::memory_order_acquire);
            Node<T>* next = current_tail->next.load(std::memory_order_acquire);
            if (current_tail == tail_.load(std::memory_order_acquire)) {
                if (next == nullptr) {
                    if (current_tail->next.compare_exchange_weak(
                            next, new_node, std::memory_order_release, std::memory_order_relaxed)) {
                        tail_.compare_exchange_weak(
                            current_tail, new_node, std::memory_order_release, std::memory_order_relaxed);
                        return;
                    }
                } else {
                    tail_.compare_exchange_weak(
                        current_tail, next, std::memory_order_release, std::memory_order_relaxed);
                }
            }
        }
    }

    std::shared_ptr<T> try_pop() {
        HazardPointerOwner hp_head;
        HazardPointerOwner hp_next;
        while (true) {
            Node<T>* current_head = head_.load(std::memory_order_acquire);
            hp_head.set_ptr(current_head);

            if (current_head != head_.load(std::memory_order_acquire)) continue; // head changed

            Node<T>* next = current_head->next.load(std::memory_order_acquire);
            if (!next) {
                // queue appears empty
                Node<T>* current_tail = tail_.load(std::memory_order_acquire);
                if (current_head == current_tail) {
                    return nullptr;
                }
                continue;
            }

            Node<T>* current_tail = tail_.load(std::memory_order_acquire);
            if (current_head == current_tail) {
                tail_.compare_exchange_weak(current_tail, next, std::memory_order_release, std::memory_order_relaxed);
                continue;
            }

            hp_next.set_ptr(next);
            if (current_head != head_.load(std::memory_order_acquire)) {
                continue;
            }
            if (current_head->next.load(std::memory_order_acquire) != next) {
                continue;
            }

            std::shared_ptr<T> res = next->data;
            if (head_.compare_exchange_weak(current_head, next, std::memory_order_release, std::memory_order_relaxed)) {
                hp_head.clear();
                reclaim_later(current_head);
                delete_nodes_with_no_hazards();
                hp_next.clear();
                return res;
            }
        }
    }

    std::shared_ptr<T> wait_pop() {
        std::shared_ptr<T> res;
        while ((res = try_pop()) == nullptr) std::this_thread::yield();
        return res;
    }

    std::shared_ptr<T> pop() { return try_pop(); }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
};