#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include "lockfreequeue.cpp"

#include "finelockqueue.cpp"
#include <random>

int main() {
    const int NUM_THREADS = 8;
    const int NUM_OPS = 500000;
    std::cout << "=== LockFreeQueue test ===" << std::endl;
    {
        LockFreeQueue<int> queue;
        std::atomic<int> produced{0}, consumed{0};
        auto start = std::chrono::high_resolution_clock::now();
        std::vector<std::thread> threads;
        // Producers
        for (int i = 0; i < NUM_THREADS/2; ++i) {
            threads.emplace_back([&queue, &produced]() {
                for (int j = 0; j < NUM_OPS; ++j) {
                    queue.push(j);
                    ++produced;
                }
            });
        }
        // Consumers
        for (int i = 0; i < NUM_THREADS/2; ++i) {
            threads.emplace_back([&queue, &consumed]() {
                int local = 0;
                while (local < NUM_OPS) {
                    auto item = queue.try_pop();
                    if (item) {
                        ++consumed;
                        ++local;
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (auto& t : threads) t.join();
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "LockFreeQueue: produced=" << produced << ", consumed=" << consumed << ", time=" << ms << " ms" << std::endl;
    }

    std::cout << "\n=== FineLockQueue test ===" << std::endl;
    {
        FineLockQueue<int> queue;
        std::atomic<int> produced{0}, consumed{0};
        auto start = std::chrono::high_resolution_clock::now();
        std::vector<std::thread> threads;
        // Producers
        for (int i = 0; i < NUM_THREADS/2; ++i) {
            threads.emplace_back([&queue, &produced]() {
                for (int j = 0; j < NUM_OPS; ++j) {
                    queue.push(j);
                    ++produced;
                }
            });
        }
        // Consumers
        for (int i = 0; i < NUM_THREADS/2; ++i) {
            threads.emplace_back([&queue, &consumed]() {
                int local = 0;
                while (local < NUM_OPS) {
                    auto item = queue.try_pop();
                    if (item) {
                        ++consumed;
                        ++local;
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (auto& t : threads) t.join();
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "FineLockQueue: produced=" << produced << ", consumed=" << consumed << ", time=" << ms << " ms" << std::endl;
    }

    std::cout << "\n=== Comparison finished ===" << std::endl;
    return 0;
}