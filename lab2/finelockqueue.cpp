#include <mutex>
#include <memory>
#include <thread>
#include <condition_variable>

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
    std::mutex head_mutex_;
    std::mutex tail_mutex_;
    std::condition_variable cv_;

public:
    FineLockQueue() {
        FineNode<T>* dummy = new FineNode<T>();
        head_ = dummy;
        tail_ = dummy;
    }

    ~FineLockQueue() {
        std::lock_guard<std::mutex> head_lock(head_mutex_);
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
        {
            std::lock_guard<std::mutex> tail_lock(tail_mutex_);
            tail_->next = new_node;
            tail_ = new_node;
        }
        cv_.notify_one();
    }

    std::shared_ptr<T> try_pop() {
        std::lock_guard<std::mutex> head_lock(head_mutex_);
        FineNode<T>* next = head_->next;
        if (next == nullptr) {
            return nullptr;
        }
        std::shared_ptr<T> res = next->data;
        FineNode<T>* old_head = head_;
        head_ = next;
        delete old_head;
        return res;
    }

    std::shared_ptr<T> wait_pop() {
        std::unique_lock<std::mutex> head_lock(head_mutex_);
        cv_.wait(head_lock, [&] { return head_->next != nullptr; });
        FineNode<T>* next = head_->next;
        std::shared_ptr<T> res = next->data;
        FineNode<T>* old_head = head_;
        head_ = next;
        head_lock.unlock();
        delete old_head;
        return res;
    }

    std::shared_ptr<T> pop() {
        return try_pop();
    }

    bool empty() {
        std::lock_guard<std::mutex> head_lock(head_mutex_);
        return head_->next == nullptr;
    }
};
