#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

class ThreadPool {
public:
    ThreadPool(size_t threads);
    ~ThreadPool();

    void enqueue(int client_socket);

    // metrics
    int active_workers() const { return active.load(); }
    size_t queue_length();

    double average_service_ms() const;

private:
    void worker();

    std::vector<std::thread> workers;
    std::queue<int> tasks;

    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;

    std::atomic<int> active;
    std::atomic<long long> total_service_ns;
    std::atomic<long long> jobs;
};

#endif
