#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sim {

class ThreadPool {
public:
    explicit ThreadPool(unsigned requestedThreads = 0) {
        unsigned hw = requestedThreads > 0 ? requestedThreads : std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        // Cap worker count so synchronization stays negligible on ~270-row grids
        const unsigned totalThreads = std::clamp(hw, 1u, 8u);
        const unsigned workersCount = totalThreads > 1 ? totalThreads - 1 : 0;

        m_workers.reserve(workersCount);
        for (unsigned i = 0; i < workersCount; ++i) {
            m_workers.emplace_back([this]() { workerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
            ++m_epoch;
        }
        m_cvStart.notify_all();
        for (auto& t : m_workers) {
            if (t.joinable()) t.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned totalThreads() const noexcept {
        return static_cast<unsigned>(m_workers.size()) + 1u;
    }

    template <typename Fn>
    void parallelFor(int begin, int end, Fn&& fn) {
        const int count = end - begin;
        if (count <= 0) return;

        if (m_workers.empty() || count < 16) {
            fn(begin, end);
            return;
        }

        const int total = static_cast<int>(totalThreads());
        const int chunks = std::min(total, count);

        std::function<void(int, int)> task = [&](int s, int e) { fn(s, e); };

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_task = &task;
            m_begin = begin;
            m_end = end;
            m_chunks = chunks;
            m_nextChunk.store(1, std::memory_order_relaxed); // Chunk 0 executed by caller
            m_activeWorkers.store(static_cast<int>(m_workers.size()), std::memory_order_relaxed);
            ++m_epoch;
        }
        m_cvStart.notify_all();

        // Caller executes chunk 0 and any remaining dynamic chunks
        executeChunk(0, begin, end, chunks, fn);
        while (true) {
            const int c = m_nextChunk.fetch_add(1, std::memory_order_relaxed);
            if (c >= chunks) break;
            executeChunk(c, begin, end, chunks, fn);
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        m_cvDone.wait(lock, [this]() {
            return m_activeWorkers.load(std::memory_order_acquire) == 0;
        });
        m_task = nullptr;
    }

private:
    template <typename Fn>
    static void executeChunk(int chunkIdx, int begin, int end, int chunks, Fn&& fn) {
        const int count = end - begin;
        const int start = begin + (chunkIdx * count) / chunks;
        const int stop = begin + ((chunkIdx + 1) * count) / chunks;
        if (start < stop) {
            fn(start, stop);
        }
    }

    void workerLoop() {
        uint64_t seenEpoch = 0;
        while (true) {
            std::function<void(int, int)>* taskPtr = nullptr;
            int begin = 0;
            int end = 0;
            int chunks = 0;

            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cvStart.wait(lock, [this, seenEpoch]() {
                    return m_stop || m_epoch != seenEpoch;
                });
                if (m_stop) return;
                seenEpoch = m_epoch;
                taskPtr = m_task;
                begin = m_begin;
                end = m_end;
                chunks = m_chunks;
            }

            if (taskPtr) {
                while (true) {
                    const int c = m_nextChunk.fetch_add(1, std::memory_order_relaxed);
                    if (c >= chunks) break;
                    executeChunk(c, begin, end, chunks, *taskPtr);
                }
            }

            if (m_activeWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_cvDone.notify_one();
            }
        }
    }

    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_cvStart;
    std::condition_variable m_cvDone;

    std::function<void(int, int)>* m_task = nullptr;
    int m_begin = 0;
    int m_end = 0;
    int m_chunks = 0;
    uint64_t m_epoch = 0;
    bool m_stop = false;

    std::atomic<int> m_nextChunk{0};
    std::atomic<int> m_activeWorkers{0};
};

} // namespace sim
