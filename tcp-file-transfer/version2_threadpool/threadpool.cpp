#include "threadpool.h"
#include <unistd.h>
#include <iostream>

extern void send_file(int client_socket);

ThreadPool::ThreadPool(size_t threads)
    : stop(false), active(0), total_service_ns(0), jobs(0) {
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back(&ThreadPool::worker, this);
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();

    for (std::thread &worker : workers) {
        worker.join();
    }
}

void ThreadPool::enqueue(int client_socket) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        tasks.push(client_socket);
    }
    condition.notify_one();
}

size_t ThreadPool::queue_length() {
    std::unique_lock<std::mutex> lock(queue_mutex);
    return tasks.size();
}

double ThreadPool::average_service_ms() const {
    long long j = jobs.load();
    if (j == 0) return 0.0;
    double ns = static_cast<double>(total_service_ns.load());
    return (ns / 1e6) / static_cast<double>(j);
}

void ThreadPool::worker() {
    while (true) {
        int client_socket;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            condition.wait(lock, [this] {
                return stop || !tasks.empty();
            });

            if (stop && tasks.empty())
                return;

            client_socket = tasks.front();
            tasks.pop();
        }

        active.fetch_add(1);
        auto t0 = std::chrono::steady_clock::now();

        send_file(client_socket);
        close(client_socket);

        auto t1 = std::chrono::steady_clock::now();
        long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        total_service_ns.fetch_add(ns);
        jobs.fetch_add(1);

        active.fetch_sub(1);

        // quick metrics print (good for screenshots)
        std::cout << "[METRICS] active=" << active.load()
                  << " queue=" << queue_length()
                  << " avg_service_ms=" << average_service_ms()
                  << "\n";
    }
}
