/**
 * @file IoService.cpp
 * @brief FastNet IO service — two-stage lock-free task dispatch implementation
 */
#include "IoService.h"
#include "EventPoller.h"
#include "MpscQueue.h"
#include "SpinLock.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <utility>

namespace FastNet {

namespace {

constexpr size_t kMaxThreadPoolSize  = 1024;
constexpr size_t kMaxPollEvents      = 10000;
constexpr int    kPollTimeoutMs      = 10;   // ms; poller thread epoll_wait timeout
constexpr size_t kMailboxDrainBudget = 256;  // max tasks to drain from mailbox per loop

std::mutex                   g_globalIoServiceMutex;
std::unique_ptr<IoService>   g_globalIoService;
size_t                       g_globalIoServiceThreadCount = 0;

size_t normalizeThreadCount(size_t requested) {
    const size_t hw = (std::max)(
        static_cast<size_t>(1),
        static_cast<size_t>(std::thread::hardware_concurrency()));
    const size_t n = (requested == 0) ? hw : requested;
    return (std::max)(static_cast<size_t>(1), (std::min)(n, kMaxThreadPoolSize));
}

} // namespace

struct IoService::Impl {
    std::unique_ptr<EventPoller> poller;
    std::vector<std::thread>     threads;
    size_t                       threadCount = 0;

    MpscQueue<Task> mailbox;

    std::vector<Task> sharedTasks;
    size_t            sharedReadIdx = 0;
    mutable SpinLock  sharedTasksLock;

    std::atomic<size_t> pendingCount{0};
    std::atomic<size_t> availableTaskCount{0};

    std::mutex              cvMutex;
    std::condition_variable taskCond;

    std::atomic<bool> running{false};
    std::atomic<bool> started{false};
    std::mutex        startMutex;

    explicit Impl(size_t requestedThreadCount)
        : poller(std::make_unique<EventPoller>()) {
        threadCount = normalizeThreadCount(requestedThreadCount);
    }

    void drainMailbox() {
        thread_local std::vector<Task> local;
        local.clear();
        local.reserve(kMailboxDrainBudget);

        const size_t drained = mailbox.drainUpTo(local, kMailboxDrainBudget);
        if (local.empty()) {
            return;
        }

        {
            SpinLockGuard g(sharedTasksLock);
            for (auto& t : local) {
                sharedTasks.push_back(std::move(t));
            }
        }

        availableTaskCount.fetch_add(drained, std::memory_order_release);
        if (drained == 1) {
            taskCond.notify_one();
        } else {
            taskCond.notify_all();
        }
    }

    size_t takeSharedTasks(std::vector<Task>& out, size_t maxTasks) {
        size_t taken = 0;
        {
            SpinLockGuard g(sharedTasksLock);
            const size_t available = sharedTasks.size() - sharedReadIdx;
            taken = (std::min)(available, maxTasks);
            for (size_t i = 0; i < taken; ++i) {
                out.push_back(std::move(sharedTasks[sharedReadIdx + i]));
            }
            sharedReadIdx += taken;
            if (sharedReadIdx > 0 && sharedReadIdx >= sharedTasks.size() / 2) {
                sharedTasks.erase(sharedTasks.begin(),
                                  sharedTasks.begin() + static_cast<ptrdiff_t>(sharedReadIdx));
                sharedReadIdx = 0;
            }
            if (taken != 0) {
                availableTaskCount.fetch_sub(taken, std::memory_order_acq_rel);
            }
        }
        return taken;
    }

    bool tryPopTask(Task& out) {
        SpinLockGuard g(sharedTasksLock);
        if (sharedReadIdx >= sharedTasks.size()) return false;
        out = std::move(sharedTasks[sharedReadIdx++]);
        if (sharedReadIdx == sharedTasks.size()) {
            sharedTasks.clear();
            sharedReadIdx = 0;
        }
        availableTaskCount.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }

    void executeTasks(std::vector<Task>& tasks) {
        for (auto& t : tasks) {
            try {
                t();
            } catch (...) {}
        }
        const size_t n = tasks.size();
        tasks.clear();

        const size_t prev = pendingCount.fetch_sub(n, std::memory_order_acq_rel);
        if (prev < n) {
            size_t expected = prev;
            while (expected > (static_cast<size_t>(-1) / 2)) {
                if (pendingCount.compare_exchange_weak(
                        expected, 0,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
                    break;
                }
            }
        }
    }

    void workerThread(size_t workerIndex) {
        const bool isPoller = (workerIndex == 0);
        std::vector<Task> batch;
        batch.reserve(kMailboxDrainBudget);

        try {
            while (true) {
                if (isPoller) {
                    drainMailbox();
                }

                takeSharedTasks(batch, kMailboxDrainBudget);
                if (!batch.empty()) {
                    executeTasks(batch);
                    continue;
                }

                if (!running.load(std::memory_order_acquire)) {
                    if (isPoller) drainMailbox();
                    while (takeSharedTasks(batch, kMailboxDrainBudget) != 0) {
                        executeTasks(batch);
                    }
                    break;
                }

                if (isPoller) {
                    poller->poll(kPollTimeoutMs);
                    drainMailbox();
                    continue;
                }

                {
                    std::unique_lock<std::mutex> lock(cvMutex);
                    taskCond.wait(lock, [this] {
                        return availableTaskCount.load(std::memory_order_acquire) > 0
                            || !running.load(std::memory_order_acquire);
                    });
                }
            }
        } catch (...) {}
    }
};

IoService::IoService(size_t threadCount)
    : impl_(std::make_unique<Impl>(threadCount)) {}

IoService::~IoService() noexcept {
    stop();
    join();
}

bool IoService::start() {
    std::lock_guard<std::mutex> lock(impl_->startMutex);
    if (impl_->started.load(std::memory_order_acquire)) {
        return true;
    }
    if (!impl_->poller || !impl_->poller->initialize(kMaxPollEvents)) {
        return false;
    }

    impl_->running.store(true, std::memory_order_release);
    impl_->started.store(true, std::memory_order_release);
    impl_->threads.clear();
    impl_->threads.reserve(impl_->threadCount);

    try {
        for (size_t i = 0; i < impl_->threadCount; ++i) {
            impl_->threads.emplace_back([this, i]() { impl_->workerThread(i); });
        }
    } catch (...) {
        impl_->running.store(false, std::memory_order_release);
        impl_->started.store(false, std::memory_order_release);
        impl_->taskCond.notify_all();
        impl_->poller->wakeup();
        for (auto& t : impl_->threads) {
            if (t.joinable()) t.join();
        }
        impl_->threads.clear();
        impl_->poller->shutdown();
        return false;
    }
    return true;
}

void IoService::stop() {
    if (!impl_->started.load(std::memory_order_acquire)) return;
    impl_->running.store(false, std::memory_order_release);
    impl_->taskCond.notify_all();
    impl_->poller->wakeup();
}

void IoService::join() {
    stop();

    std::vector<std::thread> toJoin;
    {
        std::lock_guard<std::mutex> lock(impl_->startMutex);
        if (!impl_->started.load(std::memory_order_acquire)) return;
        toJoin.swap(impl_->threads);
    }
    for (auto& t : toJoin) {
        if (t.joinable()) t.join();
    }
    std::lock_guard<std::mutex> lock(impl_->startMutex);
    impl_->poller->shutdown();
    impl_->started.store(false, std::memory_order_release);
}

void IoService::post(const Task& task) {
    if (!task) return;
    post(Task{task});
}

void IoService::post(Task&& task) {
    if (!task) return;

    impl_->mailbox.push(std::move(task));

    impl_->pendingCount.fetch_add(1, std::memory_order_release);
    if (impl_->started.load(std::memory_order_acquire)) {
        impl_->poller->wakeup();
    }
}

EventPoller& IoService::getPoller() {
    assert(impl_->started.load(std::memory_order_acquire) &&
           "IoService must be started before calling getPoller()");
    return *impl_->poller;
}

bool IoService::isRunning() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

size_t IoService::getThreadCount() const noexcept {
    return impl_->threadCount;
}

// ── Global IoService ──────────────────────────────────────────────────────────

void configureGlobalIoService(size_t threadCount) {
    std::lock_guard<std::mutex> lock(g_globalIoServiceMutex);
    g_globalIoServiceThreadCount = threadCount;
    if (g_globalIoService && g_globalIoService->isRunning()) return;
    if (!g_globalIoService ||
        g_globalIoService->getThreadCount() != normalizeThreadCount(threadCount)) {
        g_globalIoService = std::make_unique<IoService>(g_globalIoServiceThreadCount);
    }
}

IoService& getGlobalIoService() {
    std::lock_guard<std::mutex> lock(g_globalIoServiceMutex);
    if (!g_globalIoService ||
        (!g_globalIoService->isRunning() &&
         g_globalIoService->getThreadCount() != normalizeThreadCount(g_globalIoServiceThreadCount))) {
        g_globalIoService = std::make_unique<IoService>(g_globalIoServiceThreadCount);
    }
    if (!g_globalIoService->isRunning()) {
        g_globalIoService->start();
    }
    return *g_globalIoService;
}

void shutdownGlobalIoService() {
    std::unique_ptr<IoService> svc;
    {
        std::lock_guard<std::mutex> lock(g_globalIoServiceMutex);
        svc = std::move(g_globalIoService);
        g_globalIoServiceThreadCount = 0;
    }
    if (svc) {
        svc->stop();
        svc->join();
    }
}

} // namespace FastNet
