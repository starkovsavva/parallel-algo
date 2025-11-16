#include <mutex>
#include <memory>
#include <thread>

// Узел очереди
template<typename T>
struct FineNode {
    std::shared_ptr<T> data;
    FineNode<T>* next;
    FineNode() : data(nullptr), next(nullptr) {}
    explicit FineNode(T&& value) : data(std::make_shared<T>(std::move(value))), next(nullptr) {}
};

template<typename T>
class FineLockQueue {
private:
    FineNode<T>* head_;
    FineNode<T>* tail_;
    std::mutex mutex_;

public:
    FineLockQueue() {
        FineNode<T>* dummy = new FineNode<T>();
        head_ = dummy;
        tail_ = dummy;
    }

    ~FineLockQueue() {
        while (head_ != nullptr) {
            FineNode<T>* temp = head_;
            head_ = head_->next;
            delete temp;
        }
    }

    FineLockQueue(const FineLockQueue&) = delete;
    FineLockQueue& operator=(const FineLockQueue&) = delete;

    void push(T value) {
        FineNode<T>* new_node = new FineNode<T>(std::move(value));
        std::lock_guard<std::mutex> lock(mutex_);
        tail_->next = new_node;
        tail_ = new_node;
    }

    std::shared_ptr<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (head_ == tail_ || head_->next == nullptr) {
            return nullptr; // Очередь пуста
        }
        FineNode<T>* old_head = head_;
        FineNode<T>* new_head = head_->next;
        if (!new_head) {
            return nullptr;
        }
        std::shared_ptr<T> res = new_head->data;
        head_ = new_head;
        delete old_head; // Старый фиктивный узел больше не нужен
        return res;
    }

    std::shared_ptr<T> wait_pop() {
        std::shared_ptr<T> res;
        while ((res = try_pop()) == nullptr) {
            std::this_thread::yield();
        }
        return res;
    }

    std::shared_ptr<T> pop() {
        return try_pop();
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return head_ == tail_;
    }
};
