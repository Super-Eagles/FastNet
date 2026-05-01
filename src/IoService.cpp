/**
 * @file IoService.cpp
 * @brief FastNet IO service — two-stage lock-free task dispatch implementation
 *
 * Design summary (v1.5):
 *   post()       — lock-free MPSC push into mailbox_, atomic pendingCount_++,
 *                  wake the poller so mailbox work is drained promptly.
 *   Thread[0]    — IO poller + mailbox drainer.  Drains mailbox_ → sharedTasks_
 *                  under SpinLock, then polls IO events.
 *   Thread[1..N] — task workers.  SpinLock pop from sharedTasks_.  Sleep on CV
 *                  until availableTaskCount_ reports poppable work.
 */
#include "IoService.h"

#include <algorithm>
#include <memory>
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

// ── Construction / destruction ───────────────────────────────────────────────

IoService::IoService(size_t threadCount)
    : poller_(std::make_unique<EventPoller>()) {
    threadCount_ = normalizeThreadCount(threadCount);
}

IoService::~IoService() noexcept {
    stop();
    join();
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

bool IoService::start() {
    std::lock_guard<std::mutex> lock(startMutex_);
    if (started_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!poller_ || !poller_->initialize(kMaxPollEvents)) {
        return false;
    }

    running_.store(true, std::memory_order_release);
    started_.store(true, std::memory_order_release);
    threads_.clear();
    threads_.reserve(threadCount_);

    try {
        for (size_t i = 0; i < threadCount_; ++i) {
            threads_.emplace_back([this, i]() { workerThread(i); });
        }
    } catch (...) {
        running_.store(false, std::memory_order_release);
        started_.store(false, std::memory_order_release);
        taskCond_.notify_all();
        poller_->wakeup();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
        poller_->shutdown();
        return false;
    }
    return true;
}

void IoService::stop() {
    if (!started_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    // Wake all sleeping workers.
    taskCond_.notify_all();
    poller_->wakeup();
}

void IoService::join() {
    stop();

    std::vector<std::thread> toJoin;
    {
        std::lock_guard<std::mutex> lock(startMutex_);
        if (!started_.load(std::memory_order_acquire)) return;
        toJoin.swap(threads_);
    }
    for (auto& t : toJoin) {
        if (t.joinable()) t.join();
    }
    std::lock_guard<std::mutex> lock(startMutex_);
    poller_->shutdown();
    started_.store(false, std::memory_order_release);
}

// ── Task posting (producer API, any thread) ───────────────────────────────────

void IoService::post(const Task& task) {
    if (!task) return;
    post(Task{task});  // delegate to move overload
}

void IoService::post(Task&& task) {
    if (!task) return;

    mailbox_.push(std::move(task));

    // Wake the IO poller for every producer transition into the mailbox. The
    // poller coalesces wakeups internally, and workers are notified only after
    // drainMailbox() makes tasks poppable.
    pendingCount_.fetch_add(1, std::memory_order_release);
    if (started_.load(std::memory_order_acquire)) {
        poller_->wakeup();
    }
}

// ── Accessors ────────────────────────────────────────────────────────────────

EventPoller& IoService::getPoller() {
    assert(started_.load(std::memory_order_acquire) &&
           "IoService must be started before calling getPoller()");
    return *poller_;
}

bool IoService::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

size_t IoService::getThreadCount() const noexcept {
    return threadCount_;
}

// ── Internal helpers ─────────────────────────────────────────────────────────

void IoService::drainMailbox() {
    // Thread[0]-exclusive: drain MPSC mailbox into sharedTasks_ (SpinLock).
    // Batch drain: collect up to kMailboxDrainBudget items into a local vector
    // first (no lock), then splice into sharedTasks_ with a SINGLE lock acquire.
    // This reduces SpinLock contention vs. acquiring the lock for every item.
    thread_local std::vector<Task> local;
    local.clear();
    local.reserve(kMailboxDrainBudget);

    const size_t drained = mailbox_.drainUpTo(local, kMailboxDrainBudget);
    if (local.empty()) {
        return;
    }

    {
        SpinLockGuard g(sharedTasksLock_);
        for (auto& t : local) {
            sharedTasks_.push_back(std::move(t));
        }
    }

    availableTaskCount_.fetch_add(drained, std::memory_order_release);
    if (drained == 1) {
        taskCond_.notify_one();
    } else {
        taskCond_.notify_all();
    }
}

size_t IoService::takeSharedTasks(std::vector<Task>& out, size_t maxTasks) {
    size_t taken = 0;
    {
        SpinLockGuard g(sharedTasksLock_);
        const size_t available = sharedTasks_.size() - sharedReadIdx_;
        taken = (std::min)(available, maxTasks);
        for (size_t i = 0; i < taken; ++i) {
            out.push_back(std::move(sharedTasks_[sharedReadIdx_ + i]));
        }
        sharedReadIdx_ += taken;
        // Compact when half (or more) of the vector has been consumed.
        if (sharedReadIdx_ > 0 && sharedReadIdx_ >= sharedTasks_.size() / 2) {
            sharedTasks_.erase(sharedTasks_.begin(),
                               sharedTasks_.begin() + static_cast<ptrdiff_t>(sharedReadIdx_));
            sharedReadIdx_ = 0;
        }
        if (taken != 0) {
            availableTaskCount_.fetch_sub(taken, std::memory_order_acq_rel);
        }
    }
    return taken;
}

bool IoService::tryPopTask(Task& out) {
    SpinLockGuard g(sharedTasksLock_);
    if (sharedReadIdx_ >= sharedTasks_.size()) return false;
    out = std::move(sharedTasks_[sharedReadIdx_++]);
    // Compact eagerly when fully consumed.
    if (sharedReadIdx_ == sharedTasks_.size()) {
        sharedTasks_.clear();
        sharedReadIdx_ = 0;
    }
    availableTaskCount_.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

void IoService::executeTasks(std::vector<Task>& tasks) {
    for (auto& t : tasks) {
        try {
            t();
        } catch (...) {
            // Worker threads must remain joinable regardless of task exceptions.
        }
    }
    const size_t n = tasks.size();
    tasks.clear();

    // Decrement the pending counter atomically.
    // fetch_sub is unconditionally cheaper than a CAS loop: one RMW vs. N retries.
    // We then saturate to zero: if another thread already decremented below n,
    // the counter may be negative in unsigned arithmetic — clamp with a single
    // compare-and-swap only when strictly needed (fetch_sub result < n).
    const size_t prev = pendingCount_.fetch_sub(n, std::memory_order_acq_rel);
    if (prev < n) {
        // Underflow guard: reset to 0 with a single CAS (rare, so cost is fine).
        size_t expected = prev;  // prev is now the post-wrap value
        // Keep trying only while the counter is still underflowed (very short window).
        while (expected > (static_cast<size_t>(-1) / 2)) {
            if (pendingCount_.compare_exchange_weak(
                    expected, 0,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                break;
            }
        }
    }
}

// ── Worker thread ─────────────────────────────────────────────────────────────

void IoService::workerThread(size_t workerIndex) {
    const bool isPoller = (workerIndex == 0);
    std::vector<Task> batch;
    batch.reserve(kMailboxDrainBudget);

    try {
        while (true) {
            // ── Step 1: drain mailbox + run available tasks ─────────────────
            if (isPoller) {
                drainMailbox();
            }

            // Collect up to kMailboxDrainBudget tasks from the shared deque.
            takeSharedTasks(batch, kMailboxDrainBudget);
            if (!batch.empty()) {
                executeTasks(batch);
                continue;   // eagerly re-check for more work
            }

            // ── Step 2: check shutdown ──────────────────────────────────────
            if (!running_.load(std::memory_order_acquire)) {
                // Drain remainder before exit.
                if (isPoller) drainMailbox();
                while (takeSharedTasks(batch, kMailboxDrainBudget) != 0) {
                    executeTasks(batch);
                }
                break;
            }

            // ── Step 3: IO polling (poller thread) or sleep (task threads) ──
            if (isPoller) {
                // Poll with a timeout so we wake up periodically to check tasks.
                poller_->poll(kPollTimeoutMs);
                // After returning from poll(), drain the mailbox immediately
                // so that tasks posted during the poll are dispatched quickly.
                drainMailbox();
                continue;
            }

            // Task worker: sleep until sharedTasks_ has poppable work or shutdown.
            {
                std::unique_lock<std::mutex> lock(cvMutex_);
                taskCond_.wait(lock, [this] {
                    return availableTaskCount_.load(std::memory_order_acquire) > 0
                        || !running_.load(std::memory_order_acquire);
                });
            }
            // Loop back to step 1 to re-check.
        }
    } catch (...) {
        // Worker threads must remain joinable; swallow uncaught exceptions.
    }
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
