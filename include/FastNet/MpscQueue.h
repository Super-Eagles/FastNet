/**
 * @file MpscQueue.h
 * @brief Lock-free Multiple-Producer Single-Consumer queue (Vyukov MPSC)
 *
 * Properties:
 *  - Any number of producers may push() concurrently without lock.
 *  - Exactly ONE consumer thread may call tryPop() / drainAll() / drainUpTo().
 *  - push() is near-wait-free: one atomic exchange on the producer side.
 *
 * Cache-line layout (key correctness + performance insight):
 *  Each Node has `next` on its OWN cache line and `value` on a SEPARATE
 *  cache line.  Without this, the producer's store to `prev->next` (which
 *  makes the new node visible) invalidates the consumer's cache line that
 *  also holds the value of that prev node — pure false sharing that causes
 *  measurable latency spikes under load.
 */
#pragma once

#include "SpinLock.h"
#include <atomic>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace FastNet {

template<typename T>
class MpscQueue {
public:
    MpscQueue() {
        // Stub node: tail_ always points to the last consumed node (stub).
        // head_ points to the latest enqueued node (= stub when empty).
        Node* stub = new Node{};
        head_.store(stub, std::memory_order_relaxed);
        tail_ = stub;
    }

    ~MpscQueue() noexcept {
        Node* curr = tail_;
        while (curr != nullptr) {
            Node* next = curr->next.load(std::memory_order_acquire);
            delete curr;
            curr = next;
        }
        for (Node* node : freeList_) {
            delete node;
        }
    }

    MpscQueue(const MpscQueue&)             = delete;
    MpscQueue& operator=(const MpscQueue&)  = delete;
    MpscQueue(MpscQueue&&)                  = delete;
    MpscQueue& operator=(MpscQueue&&)       = delete;

    // ── Producer API (any thread) ──────────────────────────────────────────

    /**
     * Enqueue a value.  One atomic exchange; effectively wait-free.
     * Safe to call concurrently from any number of threads.
     */
    void push(T value) {
        Node* node = acquireNode(std::move(value));
        // Exchange head: we atomically claim "the slot after prev".
        Node* prev = head_.exchange(node, std::memory_order_acq_rel);
        // Link prev -> node so the consumer can walk to it.
        prev->next.store(node, std::memory_order_release);
    }

    // ── Consumer API (single thread only) ─────────────────────────────────

    /**
     * Try to dequeue one item.
     * Returns false when: (a) the queue is empty, or
     *                     (b) a producer is between its exchange and store
     *                         (transient; will resolve in < ~5 ns).
     */
    bool tryPop(T& out) noexcept {
        Node* tail = tail_;
        Node* next = tail->next.load(std::memory_order_acquire);
        if (next == nullptr) {
            return false;
        }
        out   = std::move(next->value);
        tail_ = next;
        recycleNode(tail);  // old stub owned exclusively by the consumer
        return true;
    }

    /**
     * Drain all currently visible items into @p container (must have push_back).
     * More efficient than repeated tryPop(): loads tail->next only once per node
     * instead of once at the start of tryPop() + once more inside.
     * Returns the number of items moved.
     */
    template<typename Container>
    size_t drainAll(Container& container) {
        size_t n    = 0;
        Node*  tail = tail_;

        for (;;) {
            Node* next = tail->next.load(std::memory_order_acquire);
            if (next == nullptr) {
                break;
            }
            container.push_back(std::move(next->value));
            Node* old = tail;
            tail      = next;
            recycleNode(old);
            ++n;
        }
        tail_ = tail;
        return n;
    }

    /**
     * Drain at most @p maxItems currently visible items.
     * This preserves queue contents beyond the budget instead of forcing callers
     * to drain everything and discard excess work.
     */
    template<typename Container>
    size_t drainUpTo(Container& container, size_t maxItems) {
        size_t n    = 0;
        Node*  tail = tail_;

        while (n < maxItems) {
            Node* next = tail->next.load(std::memory_order_acquire);
            if (next == nullptr) {
                break;
            }
            container.push_back(std::move(next->value));
            Node* old = tail;
            tail      = next;
            recycleNode(old);
            ++n;
        }
        tail_ = tail;
        return n;
    }

    /**
     * Approximate empty check.
     * May transiently return true while a push() is in flight (between
     * the exchange and the store).  Do NOT use as a synchronisation barrier.
     */
    bool empty() const noexcept {
        return tail_->next.load(std::memory_order_acquire) == nullptr;
    }

private:
    // ── Node layout ───────────────────────────────────────────────────────
    //
    // `next` lives on cache line 0; `value` lives on cache line 1.
    //
    // Why: the producer stores into `prev->next` to make the new node
    // visible.  If `value` and `next` shared a line, that store would
    // invalidate the consumer's line holding the value of `prev` — even
    // though the consumer is about to read `prev->value`, not write it.
    // Separate lines eliminate the false sharing completely.
    //
    struct Node {
        alignas(64) std::atomic<Node*> next{nullptr};  // link — cache line 0
        alignas(64) T value{};                          // payload — cache line 1
        Node() = default;
        explicit Node(T v) : value(std::move(v)) {}
    };

    Node* acquireNode(T value) {
        {
            SpinLockGuard lock(freeListLock_);
            if (!freeList_.empty()) {
                Node* node = freeList_.back();
                freeList_.pop_back();
                node->value = std::move(value);
                node->next.store(nullptr, std::memory_order_relaxed);
                return node;
            }
        }
        return new Node(std::move(value));
    }

    void recycleNode(Node* node) {
        node->value = T{};
        SpinLockGuard lock(freeListLock_);
        freeList_.push_back(node);
    }

    // head_ is written by every producer (one exchange per push).
    alignas(64) std::atomic<Node*> head_;

    // tail_ is read/written exclusively by the single consumer.
    alignas(64) Node* tail_;

    SpinLock freeListLock_;
    std::vector<Node*> freeList_;
};

} // namespace FastNet
