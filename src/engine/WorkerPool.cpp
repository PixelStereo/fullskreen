#include "WorkerPool.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct Job {
    const std::function<void(int)> *fn = nullptr;
    int count = 0;
    std::atomic<int> next{0}; // next index to take
    std::atomic<int> done{0}; // indices finished
    std::mutex m;
    std::condition_variable cv;

    // Takes and runs indices until none is left. True if it finished the last one.
    void work()
    {
        for (int i; (i = next.fetch_add(1)) < count;) {
            (*fn)(i);
            if (done.fetch_add(1) + 1 == count) {
                std::lock_guard<std::mutex> lk(m);
                cv.notify_all();
            }
        }
    }
};

class Pool
{
public:
    Pool()
    {
        const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
        const int n = int(std::min(16u, hw - 1));
        for (int i = 0; i < n; ++i) m_threads.emplace_back([this] { loop(); });
    }
    ~Pool()
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_quit = true;
        }
        m_cv.notify_all();
        for (auto &t : m_threads) t.join();
    }
    int size() const { return int(m_threads.size()); }

    void run(int count, const std::function<void(int)> &fn)
    {
        auto job = std::make_shared<Job>();
        job->fn = &fn;
        job->count = count;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_jobs.push_back(job);
        }
        m_cv.notify_all();
        job->work(); // the caller takes its share
        std::unique_lock<std::mutex> lk(job->m);
        job->cv.wait(lk, [&] { return job->done.load() == count; });
    }

private:
    void loop()
    {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lk(m_mutex);
                m_cv.wait(lk, [&] { return m_quit || !m_jobs.empty(); });
                if (m_quit) return;
                job = m_jobs.front();
                // Every index taken: the job leaves the queue (its last ones may still be running)
                if (job->next.load() >= job->count) {
                    m_jobs.pop_front();
                    continue;
                }
            }
            job->work();
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!m_jobs.empty() && m_jobs.front() == job) m_jobs.pop_front();
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::shared_ptr<Job>> m_jobs;
    bool m_quit = false;
};

Pool &pool()
{
    static Pool p;
    return p;
}

} // namespace

namespace workers {

void parallelFor(int count, const std::function<void(int)> &fn)
{
    if (count <= 0) return;
    if (count == 1) {
        fn(0);
        return;
    }
    pool().run(count, fn);
}

int threadCount() { return pool().size(); }

} // namespace workers
