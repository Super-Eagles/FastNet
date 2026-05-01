/**
 * @file IoService.h
 * @brief FastNet IO Service - high-performance multi-threaded event loop
 * @version 1.5.0
 *
 * Architecture changes (v1.5):
 *  - Producer side: MpscQueue<Task> replaces mutex + deque for lock-free posting.
 *  - Consumer side: SpinLock protects the secondary distribution deque
 *    (much shorter critical section than a full std::mutex).
 *  - Pending-count atomic drives worker sleep/wake via condition_variable,
 *    eliminating spurious wakeups while staying lock-free on the hot path.
 *  - Thread[0] is the dedicated IO-poller and task-mailbox drainer; it drains
 *    the MPSC mailbox into a shared SpinLock-protected deque that other workers
 *    consume.  This decouples high-frequency post() contention from dispatch.
 *
 * Threading model:
 *   Any thread  → post()         → MpscQueue (lock-free push)
 *   Thread[0]   → drainMailbox() → secondary deque (SpinLock)
 *               → poll IO events (epoll / IOCP / WSAPoll)
 *   Thread[1..N] → pop from secondary deque (SpinLock) → execute task()
 */
#pragma once

#include "Config.h"
#include "EventPoller.h"
#include "MpscQueue.h"
#include "SpinLock.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace FastNet {

using Task = std::function<void()>;

class FASTNET_API IoService {
public:
    explicit IoService(size_t threadCount = 0);
    ~IoService() noexcept;

    IoService(const IoService&)            = delete;
    IoService& operator=(const IoService&) = delete;
    IoService(IoService&&)                 = delete;
    IoService& operator=(IoService&&)      = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /** Start the IO service and all worker threads. */
    bool start();

    /** Request a graceful shutdown. */
    void stop();

    /** Block until all worker threads have exited. */
    void join();

    // ── Task submission ───────────────────────────────────────────────────

    /**
     * Post a task for execution on a worker thread.
     * Safe to call from any thread.  Lock-free on the producer side (MPSC push).
     * Workers are notified via atomic + condition_variable; no spurious wakeups.
     */
    void post(const Task& task);
    void post(Task&& task);

    // ── Accessors ─────────────────────────────────────────────────────────

    /** Must not be called before start() succeeds. */
    EventPoller& getPoller();

    bool   isRunning()     const noexcept;
    size_t getThreadCount() const noexcept;

private:
    // ── Members ───────────────────────────────────────────────────────────

    std::unique_ptr<EventPoller> poller_;
    std::vector<std::thread>     threads_;
    size_t                       threadCount_ = 0;

    // ── Task queuing (two-stage design) ───────────────────────────────────

    // Stage 1: lock-free MPSC mailbox.  All external callers push here.
    MpscQueue<Task> mailbox_;

    // Stage 2: secondary shared vector + read-cursor (ring-buffer semantics).
    // Thread[0] drains mailbox → here; workers pop from sharedReadIdx_ forward.
    // Avoids std::deque's block-list allocations on pop_front.
    std::vector<Task> sharedTasks_;
    size_t            sharedReadIdx_ = 0;
    mutable SpinLock  sharedTasksLock_;

    // Tracks the approximate number of tasks across both stages.
    // Producers increment; consumers decrement.
    // Tracks lifecycle/accounting across both stages.
    std::atomic<size_t> pendingCount_{0};

    // Tracks tasks that have already moved from the MPSC mailbox into
    // sharedTasks_ and can be popped by worker threads immediately.
    std::atomic<size_t> availableTaskCount_{0};

    // Condition variable for sleeping workers (Thread[1..N]).
    // The associated mutex is held ONLY during cv.wait(); it does NOT
    // protect sharedTasks_ (SpinLock does that).
    std::mutex              cvMutex_;
    std::condition_variable taskCond_;

    // ── Status ────────────────────────────────────────────────────────────

    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};
    std::mutex        startMutex_;

    // ── Internals ─────────────────────────────────────────────────────────

    // Drain the MPSC mailbox into sharedTasks_.  Called only by Thread[0].
    void drainMailbox();

    // Move up to maxTasks from sharedTasks_ into out.
    size_t takeSharedTasks(std::vector<Task>& out, size_t maxTasks);

    // Try to pop one task from sharedTasks_ without blocking.
    bool tryPopTask(Task& out);

    // Execute a batch of tasks.
    void executeTasks(std::vector<Task>& tasks);

    // Worker thread entry point.
    void workerThread(size_t workerIndex);
};

// ── Global IoService ──────────────────────────────────────────────────────────

FASTNET_API void      configureGlobalIoService(size_t threadCount);
FASTNET_API IoService& getGlobalIoService();
FASTNET_API void      shutdownGlobalIoService();

} // namespace FastNet
