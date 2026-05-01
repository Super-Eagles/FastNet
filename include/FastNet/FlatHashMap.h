/**
 * @file FlatHashMap.h
 * @brief Cache-friendly open-addressing hash map with Robin Hood probing
 *
 * Drop-in replacement for std::unordered_map<Key, Value> with significantly
 * better cache performance:
 *  - Flat contiguous storage (struct-of-arrays layout for hot PSL metadata).
 *  - Robin Hood insertion: bounded probe sequence lengths, average < 2 probes.
 *  - Backward-shift deletion: no tombstones, maintains good probe sequences.
 *  - Default load factor target: 75 % (resize at 3/4 capacity).
 *
 * Constraints:
 *  - Key must be equality-comparable and std::hash<Key> specialisation exists.
 *  - Does NOT guarantee iterator stability on insert/erase/rehash.
 *  - Not thread-safe; use external synchronisation (SpinLock/shared_mutex).
 */
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace FastNet {

namespace detail {

// Probe-sequence length stored per slot.  0 = empty, >0 = occupied.
// Using uint8_t limits max PSL to 255, which is ample for load factor <= 0.75.
using Psl = uint8_t;
static constexpr Psl kEmpty     = 0;
static constexpr Psl kPslMax    = std::numeric_limits<Psl>::max();

} // namespace detail

template<
    typename Key,
    typename Value,
    typename Hash    = std::hash<Key>,
    typename KeyEq   = std::equal_to<Key>
>
class FlatHashMap {
public:
    using key_type        = Key;
    using mapped_type     = Value;
    using value_type      = std::pair<const Key, Value>;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;
    using hasher          = Hash;
    using key_equal       = KeyEq;

    // ── Iterator ─────────────────────────────────────────────────────────

    class iterator {
        friend class FlatHashMap;
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = FlatHashMap::value_type;
        using difference_type   = ptrdiff_t;
        using pointer           = value_type*;
        using reference         = value_type&;

        reference operator*()  const noexcept { return *reinterpret_cast<value_type*>(&map_->kv_[idx_]); }
        pointer   operator->() const noexcept { return reinterpret_cast<value_type*>(&map_->kv_[idx_]);  }

        iterator& operator++() noexcept {
            ++idx_;
            while (idx_ < map_->capacity_ && map_->psl_[idx_] == detail::kEmpty) {
                ++idx_;
            }
            return *this;
        }
        iterator operator++(int) noexcept { auto tmp = *this; ++(*this); return tmp; }

        bool operator==(const iterator& o) const noexcept { return idx_ == o.idx_; }
        bool operator!=(const iterator& o) const noexcept { return idx_ != o.idx_; }

    private:
        iterator(FlatHashMap* m, size_t i) noexcept : map_(m), idx_(i) {}
        FlatHashMap* map_;
        size_t       idx_;
    };

    class const_iterator {
        friend class FlatHashMap;
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = FlatHashMap::value_type;
        using difference_type   = ptrdiff_t;
        using pointer           = const value_type*;
        using reference         = const value_type&;

        reference operator*()  const noexcept { return *reinterpret_cast<value_type*>(&map_->kv_[idx_]); }
        pointer   operator->() const noexcept { return reinterpret_cast<value_type*>(&map_->kv_[idx_]);  }

        const_iterator& operator++() noexcept {
            ++idx_;
            while (idx_ < map_->capacity_ && map_->psl_[idx_] == detail::kEmpty) {
                ++idx_;
            }
            return *this;
        }
        const_iterator operator++(int) noexcept { auto tmp = *this; ++(*this); return tmp; }

        bool operator==(const const_iterator& o) const noexcept { return idx_ == o.idx_; }
        bool operator!=(const const_iterator& o) const noexcept { return idx_ != o.idx_; }

    private:
        const_iterator(const FlatHashMap* m, size_t i) noexcept : map_(m), idx_(i) {}
        const FlatHashMap* map_;
        size_t             idx_;
    };

    // ── Construction / destruction ────────────────────────────────────────

    explicit FlatHashMap(size_t initialCapacity = 16,
                         const Hash&  hash = Hash{},
                         const KeyEq& eq   = KeyEq{})
        : hash_(hash), eq_(eq)
    {
        rehash(nextPow2(std::max<size_t>(initialCapacity, 4)));
    }

    FlatHashMap(const FlatHashMap&)            = default;
    FlatHashMap& operator=(const FlatHashMap&) = default;
    FlatHashMap(FlatHashMap&&) noexcept        = default;
    FlatHashMap& operator=(FlatHashMap&&) noexcept = default;

    // ── Capacity ──────────────────────────────────────────────────────────

    size_t size()     const noexcept { return size_; }
    size_t capacity() const noexcept { return capacity_; }
    bool   empty()    const noexcept { return size_ == 0; }

    void reserve(size_t n) {
        const size_t needed = (n * 4 + 2) / 3;  // n / 0.75
        if (needed > capacity_) {
            rehash(nextPow2(needed));
        }
    }

    // ── Lookup ────────────────────────────────────────────────────────────

    iterator find(const Key& key) noexcept {
        const size_t slot = findSlot(key);
        return slot == npos ? end() : iterator(this, slot);
    }

    const_iterator find(const Key& key) const noexcept {
        const size_t slot = findSlot(key);
        return slot == npos ? end() : const_iterator(this, slot);
    }

    size_t count(const Key& key) const noexcept {
        return findSlot(key) != npos ? 1 : 0;
    }

    bool contains(const Key& key) const noexcept {
        return findSlot(key) != npos;
    }

    Value& at(const Key& key) {
        const size_t slot = findSlot(key);
        if (slot == npos) { throw std::out_of_range("FlatHashMap::at: key not found"); }
        return kv_[slot].second;
    }

    const Value& at(const Key& key) const {
        const size_t slot = findSlot(key);
        if (slot == npos) { throw std::out_of_range("FlatHashMap::at: key not found"); }
        return kv_[slot].second;
    }

    Value& operator[](const Key& key) {
        return insertOrGet(key);
    }

    Value& operator[](Key&& key) {
        return insertOrGet(std::move(key));
    }

    // ── Modifiers ─────────────────────────────────────────────────────────

    std::pair<iterator, bool> insert(const value_type& kv) {
        return emplace(kv.first, kv.second);
    }

    std::pair<iterator, bool> insert(value_type&& kv) {
        return emplace(std::move(kv.first), std::move(kv.second));
    }

    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        // Construct a temporary to extract key and value.
        value_type tmp{std::forward<Args>(args)...};
        // Check for existing.
        const size_t existingSlot = findSlot(tmp.first);
        if (existingSlot != npos) {
            return {iterator(this, existingSlot), false};
        }
        growIfNeeded();
        const size_t insertedSlot = insertUnchecked(std::move(tmp.first), std::move(tmp.second));
        return {iterator(this, insertedSlot), true};
    }

    // Erase by key. Returns number of elements erased (0 or 1).
    size_t erase(const Key& key) noexcept {
        size_t slot = findSlot(key);
        if (slot == npos) return 0;
        backwardShiftErase(slot);
        --size_;
        return 1;
    }

    // Erase by iterator. Returns iterator to next element.
    iterator erase(iterator it) noexcept {
        if (it == end()) return end();
        const size_t slot = it.idx_;
        backwardShiftErase(slot);
        --size_;
        // After backward-shift, slot now holds the next element (or is empty).
        // Advance to find the next occupied slot.
        size_t next = slot;
        while (next < capacity_ && psl_[next] == detail::kEmpty) {
            ++next;
        }
        return iterator(this, next);
    }

    void clear() noexcept {
        for (size_t i = 0; i < capacity_; ++i) {
            if (psl_[i] != detail::kEmpty) {
                kv_[i].destroyPayload();  // 显式调用 Key/Value 析构
                psl_[i] = detail::kEmpty;
            }
        }
        size_ = 0;
    }

    // ── Iteration ─────────────────────────────────────────────────────────

    iterator begin() noexcept {
        size_t i = 0;
        while (i < capacity_ && psl_[i] == detail::kEmpty) ++i;
        return iterator(this, i);
    }
    iterator end() noexcept { return iterator(this, capacity_); }

    const_iterator begin() const noexcept {
        size_t i = 0;
        while (i < capacity_ && psl_[i] == detail::kEmpty) ++i;
        return const_iterator(this, i);
    }
    const_iterator end() const noexcept { return const_iterator(this, capacity_); }

    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end();   }

private:

    static constexpr size_t npos = static_cast<size_t>(-1);

    // ── Hash helpers ──────────────────────────────────────────────────────

    size_t slotOf(size_t hash) const noexcept { return hash & mask_; }

    size_t hashOf(const Key& key) const noexcept {
        // Mix the hash to improve distribution for integer keys.
        size_t h = hash_(key);
        // Fibonacci hashing for integer-like keys.
        h ^= h >> (sizeof(size_t) * 4);
        h *= (sizeof(size_t) == 8) ? 0x9e3779b97f4a7c15ULL : 0x9e3779b9U;
        h ^= h >> (sizeof(size_t) * 4);
        return h;
    }

    // ── Slot lookup ───────────────────────────────────────────────────────

    size_t findSlot(const Key& key) const noexcept {
        if (capacity_ == 0) return npos;
        const size_t h     = hashOf(key);
        size_t       slot  = slotOf(h);
        detail::Psl  dist  = 1;

        for (;;) {
            const detail::Psl psl = psl_[slot];
            if (psl == detail::kEmpty) { return npos; }
            if (psl < dist)            { return npos; } // RH invariant: if our dist > slot's dist, key not here
            if (psl == dist && eq_(kv_[slot].key(), key)) { return slot; }
            slot = slotOf(slot + 1);
            if (++dist == detail::kPslMax) { return npos; }
        }
    }

    // ── Insertion helpers ─────────────────────────────────────────────────

    template<typename K>
    Value& insertOrGet(K&& key) {
        const size_t existingSlot = findSlot(key);
        if (existingSlot != npos) {
            return kv_[existingSlot].value();
        }
        growIfNeeded();
        const size_t slot = insertUnchecked(std::forward<K>(key), Value{});
        return kv_[slot].value();
    }

    size_t insertUnchecked(Key key, Value value) {
        const size_t h    = hashOf(key);
        size_t       slot = slotOf(h);
        detail::Psl  dist = 1;
        size_t       firstInsertSlot = npos;

        for (;;) {
            detail::Psl& psl = psl_[slot];
            if (psl == detail::kEmpty) {
                // Empty slot: raw bytes are uninitialized (not yet constructed).
                // Use init() which does placement new directly — no redundant KvStorage ctor.
                kv_[slot].init(std::move(key), std::move(value));
                psl = dist;
                ++size_;
                return (firstInsertSlot == npos) ? slot : firstInsertSlot;
            }
            if (psl < dist) {
                // Robin Hood swap: evict the "rich" occupant, continue inserting the evicted pair.
                // swapWith() exchanges Key/Value in-place via std::swap — no construction or
                // destruction: both sides remain valid objects throughout.
                if (firstInsertSlot == npos) { firstInsertSlot = slot; }
                std::swap(dist, psl);
                kv_[slot].swapWith(key, value);
            }
            slot = slotOf(slot + 1);
            if (++dist == detail::kPslMax) {
                // Should not happen at 75% load factor; indicates table corruption.
                break;
            }
        }
        return firstInsertSlot;
    }

    // Backward-shift deletion: close the hole without tombstones.
    void backwardShiftErase(size_t slot) noexcept {
        size_t current = slot;
        for (;;) {
            size_t         next    = slotOf(current + 1);
            detail::Psl    nextPsl = psl_[next];
            if (nextPsl <= 1) {
                // Next slot is empty or is the head of its own chain: stop here.
                // Explicitly destroy the Key/Value in current (the erased slot).
                kv_[current].destroyPayload();
                psl_[current] = detail::kEmpty;
                return;
            }
            // Shift next → current:
            //  1. Destroy current's payload (it's the erased/overwritten slot).
            //  2. Move next's payload into current's raw storage via placement new.
            //  3. Destroy the moved-from object in next (its raw storage is now "empty").
            kv_[current].destroyPayload();    // (1) destroy occupant being overwritten
            kv_[current].moveFrom(kv_[next]); // (2) placement-new from next into current
            kv_[next].destroyPayload();        // (3) destroy the moved-from source
            psl_[current] = static_cast<detail::Psl>(nextPsl - 1);
            current = next;
        }
    }

    // ── Growth ────────────────────────────────────────────────────────────

    void growIfNeeded() {
        if (size_ + 1 > (capacity_ * 3) / 4) {
            rehash(capacity_ == 0 ? 8 : capacity_ * 2);
        }
    }

    void rehash(size_t newCapacity) {
        assert((newCapacity & (newCapacity - 1)) == 0 && "capacity must be power of 2");
        std::vector<KvStorage>    oldKv(newCapacity);
        std::vector<detail::Psl>  oldPsl(newCapacity, detail::kEmpty);
        oldKv.swap(kv_);
        oldPsl.swap(psl_);
        const size_t oldCap = capacity_;
        capacity_ = newCapacity;
        mask_     = newCapacity - 1;
        size_     = 0;

        for (size_t i = 0; i < oldCap; ++i) {
            if (oldPsl[i] != detail::kEmpty) {
                // Move Key/Value into the new table, then explicitly destroy the
                // moved-from source.  ~KvStorage() = default doesn't do this.
                insertUnchecked(std::move(oldKv[i].key()), std::move(oldKv[i].value()));
                oldKv[i].destroyPayload();  // destroy moved-from Key/Value
            }
        }
        // oldKv vector is destroyed here; KvStorage::~KvStorage() is default (no-op),
        // which is correct because we already called destroyPayload() for every occupied slot.
    }

    static size_t nextPow2(size_t n) noexcept {
        if (n == 0) return 1;
        --n;
        n |= n >> 1;  n |= n >> 2;
        n |= n >> 4;  n |= n >> 8;
        n |= n >> 16;
        if constexpr (sizeof(size_t) == 8) { n |= n >> 32; }
        return n + 1;
    }

    // ── KvStorage helpers ─────────────────────────────────────────────────

    // Extend KvStorage with typed accessors.
    // Instead of forcing std::pair<const Key, Value> which prevents Robin Hood
    // swaps, we use a mutable internal pair and provide const access as needed
    // for std::unordered_map compatibility.
    using mutable_value_type = std::pair<Key, Value>;

    struct KvStorage {
        alignas(mutable_value_type) unsigned char raw[sizeof(mutable_value_type)];

        Key&   key()   noexcept { return reinterpret_cast<mutable_value_type*>(raw)->first;  }
        Value& value() noexcept { return reinterpret_cast<mutable_value_type*>(raw)->second; }

        const Key&   key()   const noexcept { return reinterpret_cast<const mutable_value_type*>(raw)->first;  }
        const Value& value() const noexcept { return reinterpret_cast<const mutable_value_type*>(raw)->second; }

        void init(Key k, Value v) {
            new (raw) mutable_value_type{std::move(k), std::move(v)};
        }

        void swapWith(Key& k, Value& v) noexcept {
            std::swap(key(),   k);
            std::swap(value(), v);
        }

        // Move src's payload into THIS raw storage via placement new.
        // Precondition: this->raw must NOT contain a live object (caller must
        //               have called destroyPayload() first if needed).
        void moveFrom(KvStorage& src) noexcept {
            new (raw) mutable_value_type{std::move(src.key()), std::move(src.value())};
        }

        // Explicitly destroy the Key/Value pair living in raw[].
        // Must be called exactly once for every slot that has been init()'d,
        // before the slot is recycled or the KvStorage object is destroyed.
        void destroyPayload() noexcept {
            reinterpret_cast<mutable_value_type*>(raw)->~mutable_value_type();
        }

        // Default destructor intentionally does NOT destroy the payload.
        // Lifetime of the payload is managed explicitly via init() / destroyPayload()
        // so that the enclosing std::vector<KvStorage> can resize freely without
        // touching uninitialized slots (psl_[i] == kEmpty) at all.
        ~KvStorage() = default;
    };

    // ── Members ───────────────────────────────────────────────────────────

    std::vector<KvStorage>   kv_;       // flat key-value storage
    std::vector<detail::Psl> psl_;      // probe-sequence lengths (0 = empty)
    size_t                   capacity_ = 0;
    size_t                   mask_     = 0;
    size_t                   size_     = 0;
    Hash                     hash_;
    KeyEq                    eq_;
};

} // namespace FastNet
