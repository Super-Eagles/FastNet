/**
 * @file FastBuffer.h
 * @brief Small-buffer-optimized byte buffer for FastNet
 */
#pragma once

#include "Config.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastNet {

class FASTNET_API FastBuffer {
public:
    using value_type = std::uint8_t;
    using iterator = value_type*;
    using const_iterator = const value_type*;

    static constexpr size_t kStackSize = 4096;
    static constexpr size_t kMaxSize = 64 * 1024 * 1024;

    FastBuffer() = default;

    FastBuffer(const value_type* data, size_t size) {
        assign(data, size);
    }

    explicit FastBuffer(const Buffer& data) {
        assign(data);
    }

    explicit FastBuffer(std::string_view data) {
        assign(data);
    }

    FastBuffer(const FastBuffer& other) {
        assign(other.data(), other.size());
    }

    FastBuffer& operator=(const FastBuffer& other) {
        if (this != &other) {
            assign(other.data(), other.size());
        }
        return *this;
    }

    FastBuffer(FastBuffer&& other) noexcept {
        moveFrom(std::move(other));
    }

    FastBuffer& operator=(FastBuffer&& other) noexcept {
        if (this != &other) {
            releaseHeap();
            moveFrom(std::move(other));
        }
        return *this;
    }

    ~FastBuffer() {
        releaseHeap();
    }

    const value_type* data() const noexcept {
        const value_type* base = usingHeap_ ? heapBuffer_ : stackBuffer_.data();
        return base + readOffset_;
    }

    value_type* data() noexcept {
        value_type* base = usingHeap_ ? heapBuffer_ : stackBuffer_.data();
        return base + readOffset_;
    }

    iterator begin() noexcept { return data(); }
    iterator end() noexcept { return data() + size_; }
    const_iterator begin() const noexcept { return data(); }
    const_iterator end() const noexcept { return data() + size_; }
    const_iterator cbegin() const noexcept { return data(); }
    const_iterator cend() const noexcept { return data() + size_; }

    value_type& operator[](size_t index) noexcept { return data()[index]; }
    const value_type& operator[](size_t index) const noexcept { return data()[index]; }

    value_type& front() noexcept { return data()[0]; }
    const value_type& front() const noexcept { return data()[0]; }
    value_type& back() noexcept { return data()[size_ - 1]; }
    const value_type& back() const noexcept { return data()[size_ - 1]; }

    size_t size() const noexcept { return size_; }
    size_t capacity() const noexcept { return capacity_ - readOffset_; }
    constexpr size_t max_size() const noexcept { return kMaxSize; }
    bool empty() const noexcept { return size_ == 0; }
    bool usingHeapStorage() const noexcept { return usingHeap_; }

    void clear() noexcept {
        size_ = 0;
        readOffset_ = 0;
    }

    void reset() noexcept {
        size_ = 0;
        readOffset_ = 0;
        releaseHeap();
    }

    void resize(size_t newSize, value_type fill = 0) {
        validateRequestedSize(newSize);
        ensureCapacity(newSize);
        if (newSize > size_) {
            std::memset(data() + size_, fill, newSize - size_);
        }
        size_ = newSize;
    }

    void reserve(size_t newCapacity) {
        validateRequestedSize(newCapacity);
        ensureCapacity(newCapacity);
    }

    void shrink_to_fit() {
        if (!usingHeap_) {
            return;
        }

        if (size_ <= kStackSize) {
            moveToStack();
            return;
        }

        if (size_ == capacity_) {
            return;
        }

        value_type* newBuffer = static_cast<value_type*>(allocateAligned(size_));
        if (!newBuffer) {
            throw std::bad_alloc{};
        }
        std::memcpy(newBuffer, heapBuffer_, size_);
        deallocateAligned(heapBuffer_);
        heapBuffer_ = newBuffer;
        capacity_ = size_;
    }

    void push_back(value_type byte) {
        ensureCapacity(size_ + 1);
        data()[size_++] = byte;
    }

    void pop_back() noexcept {
        if (size_ > 0) {
            --size_;
        }
    }

    void append(const value_type* src, size_t len) {
        if (src == nullptr || len == 0) {
            return;
        }

        validateRequestedSize(size_ + len);
        const value_type* source = src;
        size_t sourceOffset = 0;
        const value_type* currentData = data();
        const bool overlaps = source >= currentData && source < (currentData + size_);
        if (overlaps) {
            sourceOffset = static_cast<size_t>(source - currentData);
        }

        const size_t oldSize = size_;
        ensureCapacity(oldSize + len);
        if (overlaps) {
            source = data() + sourceOffset;
        }
        std::memmove(data() + oldSize, source, len);
        size_ = oldSize + len;
    }

    void append(const FastBuffer& other) {
        append(other.data(), other.size());
    }

    void append(const Buffer& other) {
        append(other.data(), other.size());
    }

    void append(std::string_view other) {
        append(reinterpret_cast<const value_type*>(other.data()), other.size());
    }

    void append(size_t count, value_type fill) {
        if (count == 0) {
            return;
        }

        validateRequestedSize(size_ + count);
        const size_t oldSize = size_;
        ensureCapacity(oldSize + count);
        std::memset(data() + oldSize, fill, count);
        size_ = oldSize + count;
    }

    void assign(const value_type* src, size_t len) {
        if (src == nullptr || len == 0) {
            clear();
            return;
        }

        validateRequestedSize(len);
        const value_type* source = src;
        size_t sourceOffset = 0;
        const value_type* currentData = data();
        const bool overlaps = source >= currentData && source < (currentData + size_);
        if (overlaps) {
            sourceOffset = static_cast<size_t>(source - currentData);
        }

        ensureCapacity(len);
        if (overlaps) {
            source = data() + sourceOffset;
        }
        std::memmove(data(), source, len);
        size_ = len;
    }

    void assign(const Buffer& buffer) {
        assign(buffer.data(), buffer.size());
    }

    void assign(std::string_view buffer) {
        assign(reinterpret_cast<const value_type*>(buffer.data()), buffer.size());
    }

    // O(1) fast path: advance the read cursor without moving any data.
    // When wasted head space exceeds half the capacity, compact() is called
    // automatically to reclaim the space via a single memmove.
    void erase_front(size_t len) noexcept {
        if (len >= size_) {
            clear();
            return;
        }

        // Fast O(1) path: just bump the read cursor.
        readOffset_ += len;
        size_       -= len;

        // Compact when the wasted prefix exceeds half the total allocated space.
        // This keeps memory usage bounded while keeping erase_front O(1) amortised.
        if (readOffset_ > capacity_ / 2) {
            compact();
        }
    }

    // Force immediate compaction: move live data to the start of the buffer.
    // Usually not needed directly; called automatically by erase_front.
    void compact() noexcept {
        if (readOffset_ == 0 || size_ == 0) {
            readOffset_ = 0;
            return;
        }
        // Base pointer ignoring readOffset_.
        value_type* base = usingHeap_ ? heapBuffer_ : stackBuffer_.data();
        std::memmove(base, base + readOffset_, size_);
        readOffset_ = 0;
    }

    void swap(FastBuffer& other) noexcept {
        if (this == &other) {
            return;
        }
        // For heap buffers just swap pointers; for stack buffers swap raw bytes.
        if (usingHeap_ && other.usingHeap_) {
            std::swap(heapBuffer_,  other.heapBuffer_);
            std::swap(size_,        other.size_);
            std::swap(capacity_,    other.capacity_);
            std::swap(readOffset_,  other.readOffset_);  // Bug fix: must swap readOffset_ too!
            // usingHeap_ stays true for both; no need to swap it.
            return;
        }
        // Generic path: at least one uses stack storage — materialise both as
        // heap-only objects (moves them if needed), then swap pointers.
        FastBuffer tmp(std::move(other));
        other = std::move(*this);
        *this = std::move(tmp);
    }

    template<typename InputIt>
    iterator insert(const_iterator pos, InputIt first, InputIt last) {
        const size_t offset = static_cast<size_t>(pos - cbegin());
        if (offset > size_) {
            throw std::out_of_range("FastBuffer insert position out of range");
        }

        // Build insertion data without allocating a Buffer (avoid shared_ptr overhead).
        std::vector<value_type> temp(first, last);
        if (temp.empty()) {
            return data() + offset;
        }

        validateRequestedSize(size_ + temp.size());
        ensureCapacity(size_ + temp.size());

        value_type* target = data();
        std::memmove(target + offset + temp.size(),
                     target + offset,
                     size_ - offset);
        std::memcpy(target + offset, temp.data(), temp.size());
        size_ += temp.size();
        return target + offset;
    }

    std::vector<value_type> toVector() const {
        return std::vector<value_type>(cbegin(), cend());
    }

    std::string toString() const {
        return std::string(reinterpret_cast<const char*>(data()), size_);
    }

    void fromVector(const std::vector<value_type>& buffer) {
        assign(buffer);
    }

    void fromString(std::string_view buffer) {
        assign(buffer);
    }

private:
    void validateRequestedSize(size_t requestedSize) const {
        if (requestedSize > kMaxSize) {
            throw std::length_error("FastBuffer exceeds maximum size");
        }
    }

    void ensureCapacity(size_t requiredCapacity) {
        validateRequestedSize(requiredCapacity);
        // Available writable space = total capacity minus wasted prefix.
        const size_t available = capacity_ - readOffset_;
        if (requiredCapacity <= available) {
            return;
        }

        // If compacting the wasted prefix would give enough room, do that
        // instead of reallocating (avoids a heap allocation).
        if (requiredCapacity <= capacity_ && readOffset_ > 0) {
            compact();
            return;
        }

        // We need a larger buffer. Compact first so that memcpy only moves
        // the live data, then reallocate.
        compact();

        // Start from current capacity (minimum kStackSize) and double until sufficient.
        size_t newCapacity = (capacity_ == 0) ? kStackSize : capacity_;
        while (newCapacity < requiredCapacity) {
            newCapacity = (newCapacity <= kMaxSize / 2) ? newCapacity * 2 : kMaxSize;
        }
        if (newCapacity > kMaxSize) {
            newCapacity = kMaxSize;
        }
        if (newCapacity < requiredCapacity) {
            newCapacity = requiredCapacity;
        }

        value_type* newBuffer = static_cast<value_type*>(allocateAligned(newCapacity));
        if (!newBuffer) {
            throw std::bad_alloc{};
        }
        // After compact(), readOffset_ == 0 and data() == base pointer.
        if (size_ > 0) {
            std::memcpy(newBuffer, data(), size_);
        }

        releaseHeap();
        heapBuffer_ = newBuffer;
        capacity_   = newCapacity;
        usingHeap_  = true;
        // readOffset_ is already 0 after compact().
    }

    void moveToStack() noexcept {
        if (!usingHeap_) {
            return;
        }

        if (size_ > 0) {
            // data() already accounts for readOffset_; copy live bytes to stack base.
            std::memcpy(stackBuffer_.data(), data(), size_);
        }
        deallocateAligned(heapBuffer_);
        heapBuffer_  = nullptr;
        capacity_    = kStackSize;
        usingHeap_   = false;
        readOffset_  = 0;  // live data is now at stackBuffer_[0]
    }

    void moveFrom(FastBuffer&& other) noexcept {
        size_       = other.size_;
        capacity_   = other.capacity_;
        usingHeap_  = other.usingHeap_;
        readOffset_ = other.readOffset_;

        if (other.usingHeap_) {
            heapBuffer_       = other.heapBuffer_;
            other.heapBuffer_ = nullptr;
        } else if (other.size_ > 0) {
            // Copy live data from other's stack at its current read position.
            std::memcpy(stackBuffer_.data(),
                        other.stackBuffer_.data() + other.readOffset_,
                        other.size_);
            readOffset_ = 0;  // we've normalised the position in our copy
        }

        other.size_       = 0;
        other.capacity_   = kStackSize;
        other.usingHeap_  = false;
        other.readOffset_ = 0;
    }

    void releaseHeap() noexcept {
        if (usingHeap_) {
            deallocateAligned(heapBuffer_);
            heapBuffer_  = nullptr;
            usingHeap_   = false;
            capacity_    = kStackSize;
            readOffset_  = 0;
        }
    }

    static void* allocateAligned(size_t size) {
        if (size == 0) return nullptr;
#ifdef _WIN32
        return _aligned_malloc(size, 64);
#else
        // aligned_alloc requires size to be a multiple of alignment
        size_t aligned_size = (size + 63) & ~63;
        return std::aligned_alloc(64, aligned_size);
#endif
    }

    static void deallocateAligned(void* ptr) noexcept {
        if (!ptr) return;
#ifdef _WIN32
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
    }

    std::array<value_type, kStackSize> stackBuffer_{};
    value_type* heapBuffer_ = nullptr;
    size_t size_        = 0;
    size_t capacity_    = kStackSize;
    size_t readOffset_  = 0;   // O(1) erase_front cursor; normalised to 0 after compact()
    bool usingHeap_     = false;
};

inline bool operator==(const FastBuffer& lhs, const FastBuffer& rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    if (lhs.size() == 0) {
        return true;
    }
    return std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

inline bool operator!=(const FastBuffer& lhs, const FastBuffer& rhs) noexcept {
    return !(lhs == rhs);
}

inline void swap(FastBuffer& lhs, FastBuffer& rhs) noexcept {
    lhs.swap(rhs);
}

} // namespace FastNet
