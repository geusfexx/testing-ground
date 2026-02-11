#include <optional>
#include <unordered_map>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <cassert>
#include <bit>
#include <concepts>
#include <cstddef>

template <auto Num>
concept PowerOfTwoValue = std::unsigned_integral<decltype(Num)> && std::has_single_bit(Num);

template <typename T>
concept Hashable = requires(T a) {
    { std::hash<T>{}(a) } -> std::convertible_to<std::size_t>;
};


class NonCopyableNonMoveable {
public:
    NonCopyableNonMoveable() = default;

    NonCopyableNonMoveable(const NonCopyableNonMoveable&) = delete;
    NonCopyableNonMoveable& operator=(const NonCopyableNonMoveable&) = delete;

    NonCopyableNonMoveable(NonCopyableNonMoveable&&) = delete;
    NonCopyableNonMoveable& operator=(NonCopyableNonMoveable&&) = delete;
protected:
    ~NonCopyableNonMoveable() = default;
};

template <typename KeyType, typename ValueType, std::size_t Capacity = 1024>
class StrictLRU : private NonCopyableNonMoveable{    // Use EBO
public:
    static constexpr const char* name() noexcept { return "StrictLRU"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:
    static_assert(Capacity > 0);

    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = std::unordered_map<KeyType, typename cacheList::iterator>;

    void refresh(typename cacheMap::iterator it) {
        _freq_list.splice(_freq_list.begin(), _freq_list, it->second);
    }

public:
    StrictLRU() { _collection.reserve(Capacity); }

    std::optional<ValueType> get(const KeyType& key) noexcept {
        std::lock_guard<std::mutex> lock(_mtx);
        auto it = _collection.find(key);
        if (it == _collection.end()) return {};
        refresh(it);
        return it->second->second;
    }

    template <typename T>
    void put(const KeyType& key, T&& value) {
        std::lock_guard<std::mutex> lock(_mtx);
        auto it = _collection.find(key);
        if (it != _collection.end()) {
            it->second->second = std::forward<T>(value);
            refresh(it);
        } else {
            if (_freq_list.size() == Capacity) {
                _collection.erase(_freq_list.back().first);
                _freq_list.pop_back();
            }
            _freq_list.emplace_front(key, std::forward<T>(value));
            _collection[key] = _freq_list.begin();
        }
    }

private:
    std::mutex  _mtx;
    cacheList   _freq_list;         // key, value
    cacheMap    _collection;        // key, cacheList::iterator
};

template <typename KeyType, typename ValueType, std::size_t Capacity = 1024>
class SpinlockedLRU : private NonCopyableNonMoveable{    // Use EBO
public:
    static constexpr const char* name() noexcept { return "SpinlockedLRU"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:
    static_assert(Capacity > 0);

    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = std::unordered_map<KeyType, typename cacheList::iterator>;

private:    
    struct SpinLock {
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
        void lock() noexcept { while (flag.test_and_set(std::memory_order_acquire)); } // Spinlock
        void unlock() noexcept { flag.clear(std::memory_order_release); }
    };

    void refresh(typename cacheMap::iterator it) {
        _freq_list.splice(_freq_list.begin(), _freq_list, it->second);
    }

public:
    SpinlockedLRU() { _collection.reserve(Capacity); }

    std::optional<ValueType> get(const KeyType& key) noexcept {
        _lock.lock();
        auto it = _collection.find(key);
        if (it == _collection.end()) {
            _lock.unlock();
            return {};
        }
        refresh(it);
        ValueType val = it->second->second;
        _lock.unlock();
        return val;
    }

    template <typename T>
    void put(const KeyType& key, T&& value) {
        _lock.lock();
        auto it = _collection.find(key);
        if (it != _collection.end()) {
            it->second->second = std::forward<T>(value);
            refresh(it);
        } else {
            if (_freq_list.size() == Capacity) {
                _collection.erase(_freq_list.back().first);
                _freq_list.pop_back();
            }
            _freq_list.emplace_front(key, std::forward<T>(value));
            _collection[key] = _freq_list.begin();
        }
        _lock.unlock();
    }

private:
    SpinLock    _lock;
    cacheList   _freq_list;         // key, value
    cacheMap    _collection;        // key, cacheList::iterator
};


/* It has:  False Sharing resolved with alignas
*           array
*           acq-rel fence
*           division using bitwise "AND"
*/
template <typename ValueType, std::size_t Capacity>
requires PowerOfTwoValue<Capacity>
class MPSC_TraceBuffer : private NonCopyableNonMoveable{    // Use EBO
public:
    static constexpr const char* name() noexcept { return "MPSC_TraceBuffer"; }
    using value_type = ValueType;

private:
    static constexpr std::size_t CacheLine = 64; // Some hardcode :)
    static constexpr std::size_t Mask = Capacity - 1;
    static_assert(std::has_single_bit(Capacity), "Capacity must be power of 2");

public:
    bool isItTime() const noexcept {
        return (tail - head > (Capacity / 2));
    }

    bool push(const ValueType& value) {
        std::size_t curr_t = tail.load(std::memory_order_relaxed);
        while (true) { // CAS
            if (((curr_t + 1) & Mask) == head_cache) {
                head_cache = head.load(std::memory_order_acquire);
                if (((curr_t + 1) & Mask) == head_cache) {
                    return false;
                }
            }

            // Write
            if (tail.compare_exchange_weak(curr_t, (curr_t + 1) & Mask,
                                         std::memory_order_release,         // MB off
                                         std::memory_order_relaxed)) {
                buffer[curr_t] = value;
                return true;
            }
        }
    }

    bool pop(ValueType& value) {
        const std::size_t curr_h = head.load(std::memory_order_relaxed);

        if (curr_h == tail_cache) {
            tail_cache = tail.load(std::memory_order_acquire); // MB on
            if (curr_h == tail_cache) return false;
        }

        value = buffer[curr_h];
        head.store((curr_h + 1) & Mask, std::memory_order_release); // MB off

        return true;
    }

private:
    alignas(CacheLine) ValueType buffer[Capacity];

    // Producer's group
    alignas(CacheLine) std::atomic<std::size_t> tail{0};
    std::size_t head_cache{0}; // local

    // Consumer's group
    alignas(CacheLine) std::atomic<std::size_t> head{0};
    std::size_t tail_cache{0}; // local
};


// TODO     there are some points to optimize left. For example:
//          sharding, changing get() logic or using flat map
//          I guess, Adaptive Locking / Dynamic Policy Switching may be useful,
//          but it isn't needed without factual demands
//
// TODO     sharding            ******      Done
// TODO     using flat map      ******      Done
template <typename KeyType, typename ValueType, std::size_t Capacity = 1024>
class DeferredLRU : private NonCopyableNonMoveable{    // Use EBO
public:
    static constexpr const char* name() noexcept { return "DeferredLRU"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:
    static_assert(Capacity > 0);

    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = std::unordered_map<KeyType, typename cacheList::iterator>;
    using ringBuffer = MPSC_TraceBuffer<KeyType, Capacity / 4>;

private:
    void apply_updates() {

        KeyType key_to_refresh;
        while (_update_buffer.pop(key_to_refresh)) { // It is safe because value is stored in _collection
            auto it = _collection.find(key_to_refresh);
            if (it != _collection.end()) {
                _freq_list.splice(_freq_list.begin(), _freq_list, it->second);
            }
        }
    }

public:
    DeferredLRU() { _collection.reserve(Capacity); }

    std::optional<ValueType> get(const KeyType& key) noexcept {
        std::shared_lock lock(_rw_mtx);

        auto it = _collection.find(key);
        if (it == _collection.end()) return {};

        // Admission of losses
        // Accumulate updates
        _update_buffer.push(key);

        return it->second->second;
    }

    template <typename T>
    void put(const KeyType& key, T&& value) {
        std::unique_lock<std::shared_mutex> lock(_rw_mtx);

        if (_update_buffer.isItTime()) {
            apply_updates(); // Apply cummulative updates by writers
        }

        auto it = _collection.find(key);
        if (it != _collection.end()) {
            it->second->second = std::forward<T>(value);
        } else {
            if (_freq_list.size() == Capacity) {
                apply_updates(); // Emergency apply

                _collection.erase(_freq_list.back().first);
                _freq_list.pop_back();
            }
            _freq_list.emplace_front(key, std::forward<T>(value));
            _collection[key] = _freq_list.begin();
        }
    }

private:
    ringBuffer          _update_buffer;
    cacheList           _freq_list;         // key, value
    cacheMap            _collection;        // key, cacheList::iterator
    std::shared_mutex   _rw_mtx;
};

template <typename KeyType, typename ValueType, std::size_t Capacity = 1024>
requires PowerOfTwoValue<Capacity>
class LinearFlatMap : private NonCopyableNonMoveable { // Open Addressing table with Linear Probing
    struct Entry {
        KeyType key;
        ValueType value;
        bool occupied = false;
        bool deleted = false;   // Tombstone
    };

public:
    static constexpr const char* name() noexcept { return "LinearFlatMap"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:

    static_assert(std::has_single_bit(Capacity), "Capacity must be power of 2");
    static constexpr std::size_t TableSize = Capacity * 2; // Load factor 0.5
    static constexpr std::size_t Mask = TableSize - 1;

public:
    LinearFlatMap() : _table(std::make_unique<Entry[]>(TableSize)) {}

    ValueType* find(const KeyType& key) const {
        std::size_t hash = std::hash<KeyType>{}(key) & Mask;

        for (std::size_t i = 0; i < TableSize; ++i) {
            std::size_t idx = (hash + i) & Mask;

            if (!_table[idx].occupied && !_table[idx].deleted) return nullptr;
            if (_table[idx].occupied && _table[idx].key == key) return &_table[idx].value;
        }

        return nullptr;
    }

    void insert(const KeyType& key, ValueType val) {
        std::size_t hash = std::hash<KeyType>{}(key) & Mask;
        int first_del_idx_wth_same_key = -1;

        for (std::size_t i = 0; i < TableSize; ++i) {
            std::size_t idx = (hash + i) & Mask;

            if (_table[idx].occupied && _table[idx].key == key) {
                _table[idx].value = std::move(val);
                return;
            }

            if (!_table[idx].occupied) {
                std::size_t target = (first_del_idx_wth_same_key != -1) ? first_del_idx_wth_same_key : idx;

                _table[target].key = key;
                _table[target].value = std::move(val);
                _table[target].occupied = true;
                _table[target].deleted = false;
                return;
            }

            if (_table[idx].deleted && first_del_idx_wth_same_key == -1) first_del_idx_wth_same_key = idx;
        }

        return;
    }

    void erase(const KeyType& key) {
        std::size_t hash = std::hash<KeyType>{}(key) & Mask;

        for (std::size_t i = 0; i < TableSize; ++i) {
            std::size_t idx = (hash + i) & Mask;

            if (!_table[idx].occupied && !_table[idx].deleted) return;
            if (_table[idx].occupied && _table[idx].key == key) {
                _table[idx].occupied = false;
                _table[idx].deleted = true;
                return;
            }
        }

        return;
    }

private:
    std::unique_ptr<Entry[]> _table;
};

template <typename KeyType, typename ValueType, std::size_t Capacity = 1024>
class DeferredFlatLRU : private NonCopyableNonMoveable{    // Use EBO
public:
    static constexpr const char* name() noexcept { return "DeferredFlatLRU"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:
    static_assert(Capacity > 0);

    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = LinearFlatMap<KeyType, typename cacheList::iterator, Capacity>;
    using ringBuffer = MPSC_TraceBuffer<KeyType, Capacity / 4>;

private:
    void apply_updates() {

        KeyType key_to_refresh;
        while (_update_buffer.pop(key_to_refresh)) { // It is safe because value is stored in _collection
            auto it = _collection.find(key_to_refresh);
            if (it) {
                _freq_list.splice(_freq_list.begin(), _freq_list, *it);
            }
        }
    }

public:

    std::optional<ValueType> get(const KeyType& key) noexcept {
        std::shared_lock lock(_rw_mtx);

        auto it = _collection.find(key);
        if (!it) return {};

        // Admission of losses
        // Accumulate updates
        _update_buffer.push(key);

        return (*it)->second;
    }

    template <typename T>
    void put(const KeyType& key, T&& value) {
        std::unique_lock<std::shared_mutex> lock(_rw_mtx);

        if (_update_buffer.isItTime()) {
            apply_updates(); // Apply cummulative updates by writers
        }

        auto it = _collection.find(key);
        if (it) {
            (*it)->second = std::forward<T>(value);
        } else {
            if (_freq_list.size() == Capacity) {
                apply_updates(); // Emergency apply

                _collection.erase(_freq_list.back().first);
                _freq_list.pop_back();
            }
            _freq_list.emplace_front(key, std::forward<T>(value));
            _collection.insert(key, _freq_list.begin());
        }
    }

private:
    ringBuffer          _update_buffer;
    cacheList           _freq_list;         // key, value
    cacheMap            _collection;        // key, cacheList::iterator
    std::shared_mutex   _rw_mtx;
};


//  Wrapper for SharedLRU
template <template<typename, typename, std::size_t> class CacheImpl,
    typename KeyType, typename ValueType,
    std::size_t TotalCapacity = 2 * 1024,
    std::size_t ShardsCount = 16>
requires PowerOfTwoValue<ShardsCount>
class ShardedCache : private NonCopyableNonMoveable {
    static constexpr std::size_t ShardCapacity = TotalCapacity / ShardsCount;
    static_assert(ShardCapacity >= 64, "Shard capacity too small!");
    using Cache = CacheImpl<KeyType, ValueType, ShardCapacity>;

public:
    static constexpr std::string name() noexcept {
        return "Sharded<" + std::string(Cache::name()) + ">";
    }

    using value_type = ValueType;
    using key_type = KeyType;

private:
    static constexpr std::size_t Mask = ShardsCount - 1;
    static_assert(TotalCapacity > 0, "TotalCapacity must be > 0");
    static_assert(ShardsCount > 0, "ShardsCount must be > 0");
    static_assert(std::has_single_bit(ShardsCount), "ShardsCount must be power of 2");
    static_assert(std::has_single_bit(alignof(Cache)), "Alignment must be power of 2");

    std::size_t get_shard_idx(const KeyType& key) const noexcept {
        return std::hash<KeyType>{}(key) & Mask;
    }

public:
    ShardedCache() {
        _shards.reserve(ShardsCount);
        for (std::size_t i = 0; i < ShardsCount; ++i) {
            _shards.emplace_back(std::make_unique<Cache>());
        }
    }

    std::optional<ValueType> get(const KeyType& key) noexcept {
        return _shards[get_shard_idx(key)]->get(key);
    }

    template <typename T>
    void put(const KeyType& key, T&& value) {
        _shards[get_shard_idx(key)]->put(key, std::forward<T>(value));
    }

private:
    std::vector<std::unique_ptr<Cache>> _shards;
};


template <typename ValueType, std::size_t Capacity>
requires PowerOfTwoValue<Capacity>
class SPSC_RingBufferUltraFast {

    static constexpr std::size_t CacheLine = 64; // Some hardcode :)
    static_assert(std::has_single_bit(Capacity), "ShardsCount must be power of 2");
    std::size_t increment(std::size_t i) const noexcept {
        return (i + 1) & (Capacity - 1);
    }

public:
    using value_type = ValueType;

public:
    bool isItTime() const noexcept {
        return (tail.load(std::memory_order_relaxed) - head_cache > (Capacity / 2));
    }

    [[nodiscard("SPSC push failed: buffer is full, data will be lost!")]]
    bool push(const ValueType& value) {
        const std::size_t curr_t = tail.load(std::memory_order_relaxed);

        if (increment(curr_t) == head_cache) {
            head_cache = head.load(std::memory_order_acquire); // MB on
            if (increment(curr_t) == head_cache) return false;
        }

        buffer[curr_t] = value;
        tail.store(increment(curr_t), std::memory_order_release); // MB off

        return true;
    }

    [[nodiscard("SPSC pop failed: buffer is empty or update deferred, data will be lost!")]]
    bool pop(ValueType& value) {
        const std::size_t curr_h = head.load(std::memory_order_relaxed);

        if (curr_h == tail_cache) {
            tail_cache = tail.load(std::memory_order_acquire); // MB on
            if (curr_h == tail_cache) return false;
        }

        value = buffer[curr_h];
        head.store(increment(curr_h), std::memory_order_release); // MB off

        return true;
    }

private:
    alignas(CacheLine) ValueType buffer[Capacity];

    // Producer's group
    alignas(CacheLine) std::atomic<std::size_t> tail{0};
    std::size_t head_cache{0}; // local

    // Consumer's group
    alignas(CacheLine) std::atomic<std::size_t> head{0};
    std::size_t tail_cache{0}; // local
};

template <typename KeyType, typename ValueType, std::size_t Capacity = 1024, std::size_t MaxThreads = 16>
requires PowerOfTwoValue<MaxThreads>
class Lv1_bdFlatLRU : private NonCopyableNonMoveable {
public:
    static constexpr const char* name() noexcept { return "SPSCBuffer_DeferredFlatLRU"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:
    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = LinearFlatMap<KeyType, typename cacheList::iterator, Capacity>;
    using SPSCBuffer = SPSC_RingBufferUltraFast<KeyType, Capacity / (4 * MaxThreads)>;

    static constexpr std::size_t CacheLine = 64; // Some hardcode :)
    static_assert(std::has_single_bit(MaxThreads), "MaxThreads must be a power of 2!");

private:
    static std::size_t get_thread_id() {
        static std::atomic<std::size_t> counter{0};

        // rollcall
        thread_local std::size_t id = counter.fetch_add(1, std::memory_order_relaxed) & (MaxThreads - 1);
        return id;
    }

    void apply_updates() {
        uint64_t mask = _dirty_mask.exchange(0, std::memory_order_acquire);
        if (mask == 0) return;

        // Iterate only for set bits
        while (mask > 0) {
            int idx = std::countr_zero(mask); // Index by set bit

            KeyType key_to_refresh;
            while (_update_buffers[idx].pop(key_to_refresh)) {
                auto it = _collection.find(key_to_refresh);
                if (it) {
                    _freq_list.splice(_freq_list.begin(), _freq_list, *it);
                }
            }

            mask &= (mask - 1); // Next bit
        }
    }

public:
    std::optional<ValueType> get(const KeyType& key) noexcept {
        std::shared_lock lock(_rw_mtx);

        auto it = _collection.find(key);
        if (!it) return {};

        auto tid = get_thread_id();
        if (_update_buffers[tid].push(key)) {
            _dirty_mask.fetch_or(1ULL << tid, std::memory_order_relaxed); // Set bit in mask
        }

        return (*it)->second;
    }

    template <typename T>
    void put(const KeyType& key, T&& value) {
        std::unique_lock<std::shared_mutex> lock(_rw_mtx);

        if (_update_buffers[get_thread_id()].isItTime()) {
            apply_updates(); // Apply cummulative updates by writers
        }

        auto it = _collection.find(key);
        if (it) {
            (*it)->second = std::forward<T>(value);
        } else {
            if (_freq_list.size() == Capacity) {
                apply_updates(); // Emergency apply

                _collection.erase(_freq_list.back().first);
                _freq_list.pop_back();
            }
            _freq_list.emplace_front(key, std::forward<T>(value));
            _collection.insert(key, _freq_list.begin());
        }
    }

private:
    alignas(CacheLine) SPSCBuffer _update_buffers[MaxThreads];
    alignas(CacheLine) std::atomic<uint64_t> _dirty_mask{0};

    cacheList           _freq_list;
    cacheMap            _collection;
    std::shared_mutex   _rw_mtx;
};

template <typename KeyType, typename ValueType, std::size_t Capacity = 1024, std::size_t MaxThreads = 16>
requires PowerOfTwoValue<MaxThreads>
class Lv2_bdFlatLRU : private NonCopyableNonMoveable {
public:
    static constexpr const char* name() noexcept { return "Lvl2_SPSCBuffer_DeferredFlatLRU"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:
    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = LinearFlatMap<KeyType, typename cacheList::iterator, Capacity>;
    using SPSCBuffer = SPSC_RingBufferUltraFast<KeyType, Capacity / (4 * MaxThreads)>;

    static constexpr std::size_t CacheLine = 64; // Some hardcode :)
    static_assert(std::has_single_bit(MaxThreads), "MaxThreads must be a power of 2!");

private:
    struct alignas(64) PaddedSPSC : public SPSCBuffer {
        char padding[64 - (sizeof(SPSCBuffer) % 64)];
    };

private:
    static std::size_t get_thread_id() {
        static std::atomic<std::size_t> counter{0};

        // rollcall
        thread_local std::size_t id = std::numeric_limits<std::size_t>::max();

        if (id == std::numeric_limits<std::size_t>::max()) [[unlikely]] {
            id = counter.fetch_add(1, std::memory_order_relaxed) & (MaxThreads - 1);
        }

        return id;
    }

    void apply_updates() {
        uint64_t mask = _dirty_mask.exchange(0, std::memory_order_acquire);
        if (mask == 0) return;

        // Iterate only for set bits
        while (mask > 0) {
            int idx = std::countr_zero(mask); // Index by set bit

            KeyType key_to_refresh;
            while (_update_buffers[idx].pop(key_to_refresh)) {
                auto it = _collection.find(key_to_refresh);
                if (it) {
                    __builtin_prefetch(&(*it), 1, 3); // Doubtful, but okey :) It designed for heavy payload

                    _freq_list.splice(_freq_list.begin(), _freq_list, *it);
                }
            }

            mask &= (mask - 1); // Next bit
        }
    }

public:
    std::optional<ValueType> get(const KeyType& key) noexcept {
        std::shared_lock lock(_rw_mtx);

        auto it = _collection.find(key);
        if (!it) [[unlikely]] return {};

        __builtin_prefetch(&(*it)->second, 0, 1); // Doubtful, but okey :) It designed for heavy payload

        auto tid = get_thread_id();
        if (_update_buffers[tid].push(key)) {
            if (!(_dirty_mask.load(std::memory_order_relaxed) & (1ULL << tid))) {   // Test
                _dirty_mask.fetch_or(1ULL << tid, std::memory_order_relaxed);       // Test & Set bit in mask
            }
        }

        return (*it)->second;
    }

    template <typename T>
    void put(const KeyType& key, T&& value) {
        std::unique_lock<std::shared_mutex> lock(_rw_mtx);

        if (_dirty_mask.load(std::memory_order_relaxed)) {
            apply_updates(); // Apply cummulative updates by writers
        }

        auto it = _collection.find(key);
        if (it) {
            (*it)->second = std::forward<T>(value);
        } else {
            if (_freq_list.size() == Capacity) {
                apply_updates(); // Emergency apply

                _collection.erase(_freq_list.back().first);
                _freq_list.pop_back();
            }
            _freq_list.emplace_front(key, std::forward<T>(value));
            _collection.insert(key, _freq_list.begin());
        }
    }

private:
    alignas(CacheLine) PaddedSPSC _update_buffers[MaxThreads];
    alignas(CacheLine) std::atomic<uint64_t> _dirty_mask{0};

    cacheList           _freq_list;
    cacheMap            _collection;
    std::shared_mutex   _rw_mtx;
};



/*
*   TODO:   Summary       combine list logic into FlatMap as one _collection
*   TODO:                       _collection.move_to_front(...) in LRU                                       ******      Done
*   TODO:                       enum class SlotState : uint8_t { Empty = 0, Occupied = 1, Deleted = 2 };    ******      Done
*   TODO:                       _collection.get_tail()              ******      Done
*   TODO:                       _collection.find_index(key)         ******      Done
*   TODO:                       _collection.assign_slot(key);       ******      Done
*   TODO:                       get_hash_idx & next_slot            ******      Done
*/

template <Hashable KeyType, typename ValueType, std::size_t Capacity = 1024>
requires PowerOfTwoValue<Capacity>
class LinkedFlatMap : private NonCopyableNonMoveable { // Open Addressing table with Linear Probing
public:
    using index_type = std::conditional_t<(Capacity <= 65535), uint16_t, uint32_t>;
    static constexpr index_type NullIdx = std::numeric_limits<index_type>::max();
    static constexpr std::size_t CacheLine = 64; // Some hardcode :)

    enum class slot_state : uint8_t { Empty = 0, Occupied = 1, Deleted = 2 };

    static constexpr const char* name() noexcept { return "LinkedFlatMap"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:
    struct Entry {
        value_type value;                       // sizeof(ValueType)
        key_type   key;                         // 4 or 8 bytes

        // meta
        uint32_t   gen = 0;                     // 4 bytes // ABA protection
        index_type next = NullIdx;              // 2 or 4 bytes
        index_type prev = NullIdx;              // 2 or 4 bytes
        slot_state state = slot_state::Empty;   // 1 byte
    };

public:
    const Entry& get_entry(index_type idx) const noexcept {
        return _table[idx];
    }

/*    static_assert((sizeof(Entry) & (CacheLine - 1)) == 0,
        "Entry crosses cache line boundary unoptimally!");
*/
private:
    static_assert(std::has_single_bit(Capacity), "Capacity must be power of 2");
    static constexpr std::size_t TableSize = Capacity * 2; // Load factor 0.5
    static_assert(TableSize == Capacity * 2, "Load factor must be 0.5");
    static constexpr std::size_t Mask = TableSize - 1;

private:

    struct LookupResult {
        ValueType* ptr;
        index_type idx;
        uint32_t   gen;
        bool       found;
    };

    std::size_t calculate_hash_idx(const KeyType& key) const noexcept {
        return std::hash<KeyType>{}(key) & Mask;
    }

    std::size_t next_slot(std::size_t current_idx) const noexcept {
        return (current_idx + 1) & Mask;
    }

    void detach(const index_type& idx) {
        Entry& current = _table[idx];
        
        if (current.next != NullIdx) _table[current.next].prev = current.prev;
        else _tail = current.prev;
        
        if (current.prev != NullIdx) _table[current.prev].next = current.next;
        else _head = current.next;
        
        current.next = NullIdx;
        current.prev = NullIdx;
    }

    void push_front(index_type idx) {
        auto& current = _table[idx];
        current.next = _head;
        current.prev = NullIdx;

        if (_head != NullIdx) { _table[_head].prev = idx; }
        _head = idx;

        if (_tail == NullIdx) _tail = idx;
    }

public:
    LinkedFlatMap() : _table(std::make_unique<Entry[]>(TableSize)) {}

    bool is_occupied(index_type idx) const noexcept {
        return _table[idx].state == slot_state::Occupied;
    }

    bool is_valid_gen(index_type idx, uint32_t gen) const noexcept {
        return _table[idx].state == slot_state::Occupied && _table[idx].gen == gen;
    }

    std::size_t size() { return _size; }
    index_type get_tail() { return _tail; }
    index_type get_head() const noexcept { return _head; }

    LookupResult lookup(const KeyType& key) const {
        std::size_t idx = calculate_hash_idx(key);
        index_type first_del = NullIdx;

        while (true) {
            const auto& current = _table[idx];

            if (current.state == slot_state::Empty) {
                index_type target = (first_del != NullIdx) ? first_del : static_cast<index_type>(idx);
                return {nullptr, target, 0, false};
            }

            if (current.state == slot_state::Deleted && first_del == NullIdx) {
                first_del = static_cast<index_type>(idx);
            }

            if (current.state == slot_state::Occupied && current.key == key) {
                return {const_cast<ValueType*>(&current.value), static_cast<index_type>(idx), current.gen, true};
            }

            idx = next_slot(idx);
        }

        // It can loop for eternity only if Load Factor > 1.0 (all slots are Occupied or Deleted)
        // But since it has Capacity * 2 and there is no deletion of Empty slots, this is impossible.
        assert(false && "LinkedFlatMap table size overflow or corrupted logic");
        __builtin_unreachable(); // I'm sure
    }

    template <typename... Args>
    void emplace_at(index_type idx, const KeyType& key, Args&&... args) {
        auto& current = _table[idx];
        current.key = key;
        current.gen++;

        new (&current.value) value_type(std::forward<Args>(args)...); // Memory had been allocated by assign_slot

        current.state = slot_state::Occupied;
        _size++;
    }

    index_type assign_slot(const key_type& key) {
        std::size_t idx = calculate_hash_idx(key);
        index_type first_del_idx_wth_same_key = NullIdx;

        while (true) {
            if (_table[idx].state == slot_state::Empty) {
                return (first_del_idx_wth_same_key != NullIdx) ? first_del_idx_wth_same_key
                                                                             : static_cast<index_type>(idx);
            }

            if (_table[idx].state == slot_state::Deleted && first_del_idx_wth_same_key == NullIdx) {
                first_del_idx_wth_same_key = static_cast<index_type>(idx);
            }

            idx = next_slot(idx);
        }

        assert(false && "LinkedFlatMap table size overflow or corrupted logic");
        __builtin_unreachable(); // I'm sure
    }

    void move_to_front(index_type idx) {
        if (idx == _head || idx == NullIdx) return;

        Entry& curr = _table[idx];
        if (curr.next != NullIdx) __builtin_prefetch(&_table[curr.next], 1, 3);
        if (curr.prev != NullIdx) __builtin_prefetch(&_table[curr.prev], 1, 3);

        detach(idx);

        push_front(idx);
    }

    void erase_index(const index_type& idx) {
        if (idx == NullIdx || _table[idx].state != slot_state::Occupied) return;

        detach(idx);

        _table[idx].value.~ValueType(); // Due to placement new
        _table[idx].state = slot_state::Deleted;
        _size--;
    }

private:
    std::unique_ptr<Entry[]> _table;
    index_type _head = NullIdx;
    index_type _tail = NullIdx;
    std::size_t _size = 0;
};


template <Hashable KeyType, typename ValueType, std::size_t Capacity = 4 * 1024, std::size_t MaxThreads = 32>
requires PowerOfTwoValue<MaxThreads>
class Lv3_bdFlatLRU : private NonCopyableNonMoveable {
public:
    static constexpr const char* name() noexcept { return "Lv3_SPSCBuffer_DeferredFlatLRU"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:
    using cacheMap = LinkedFlatMap<KeyType, ValueType, Capacity>;

    struct UpdateOp {
        cacheMap::index_type    idx;
        uint32_t                gen;
    };

    using SPSCBuffer = SPSC_RingBufferUltraFast<UpdateOp, Capacity / (4 * MaxThreads)>;

    static constexpr std::size_t CacheLine = 64; // Some hardcode :)
    static_assert(std::has_single_bit(MaxThreads), "MaxThreads must be a power of 2!");

private:
    struct alignas(CacheLine) PaddedSPSC : public SPSCBuffer {
        char padding[CacheLine - (sizeof(SPSCBuffer) & (CacheLine - 1))];
    };

private:

    static std::size_t get_thread_id() {
        static std::atomic<std::size_t> counter{0};

        // rollcall
        thread_local std::size_t id = std::numeric_limits<std::size_t>::max();

        if (id == std::numeric_limits<std::size_t>::max()) [[unlikely]] {
            id = counter.fetch_add(1, std::memory_order_relaxed) & (MaxThreads - 1);
        }

        return id;
    }

    void apply_updates() {
        uint64_t mask = _dirty_mask.exchange(0, std::memory_order_acquire);
        if (mask == 0) return;

        // Iterate only for set bits
        while (mask > 0) {
            int buf_idx = std::countr_zero(mask); // Index by set bit
            UpdateOp op;

            while (_update_buffers[buf_idx].pop(op)) {
//                auto res = _collection.lookup(key_from_buffer);       Has been optimised
                if (_collection.is_valid_gen(op.idx, op.gen)) {
                    __builtin_prefetch(&_collection.get_entry(_collection.get_head()), 1, 3);
                    _collection.move_to_front(op.idx);
                }
            }

            mask &= (mask - 1); // Next bit
        }
    }

public:
    std::optional<ValueType> get(const KeyType& key) noexcept {
        std::shared_lock lock(_rw_mtx);

        auto res = _collection.lookup(key);
        if (!res.found) [[unlikely]] return {};

        __builtin_prefetch(&_collection.get_entry(res.idx), 1, 3);

        auto tid = get_thread_id();
        if (_update_buffers[tid].push({res.idx, res.gen})) {
            if (!(_dirty_mask.load(std::memory_order_relaxed) & (1ULL << tid))) {   // Test
                _dirty_mask.fetch_or(1ULL << tid, std::memory_order_release);       // Test & Set bit in mask
            }
        }

        return *(res.ptr);
    }

    template <typename T>
    void put(const KeyType& key, T&& value) {
        std::unique_lock lock(_rw_mtx);

        if (_dirty_mask.load(std::memory_order_relaxed)) {
            apply_updates();
        }

        auto res = _collection.lookup(key);

        if (res.found) {
            *(res.ptr) = std::forward<T>(value);
            _collection.move_to_front(res.idx);
        } else {
            if (_collection.size() >= Capacity) {
                if (_dirty_mask.load(std::memory_order_relaxed)) {
                    apply_updates();
                }

                _collection.erase_index(_collection.get_tail());
                res.idx = _collection.assign_slot(key);
            }

            _collection.emplace_at(res.idx, key, std::forward<T>(value));
            _collection.move_to_front(res.idx);
        }
    }

private:
    alignas(CacheLine) PaddedSPSC               _update_buffers[MaxThreads];
    alignas(CacheLine) std::atomic<uint64_t>    _dirty_mask{0};

    cacheMap            _collection;
    std::shared_mutex   _rw_mtx;
};


template <Hashable KeyType, typename ValueType, std::size_t Capacity = 1024>
requires PowerOfTwoValue<Capacity>
class Lv2_LinkedFlatMap : private NonCopyableNonMoveable { // Open Addressing table with Linear Probing
public:
    using index_type = std::conditional_t<(Capacity <= 65535), uint16_t, uint32_t>;
    static constexpr index_type NullIdx = std::numeric_limits<index_type>::max();
    static constexpr std::size_t CacheLine = 64; // Some hardcode :)

    enum class slot_state : uint8_t { Empty = 0, Occupied = 1, Deleted = 2 };

    static constexpr const char* name() noexcept { return "Lv2_LinkedFlatMap"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:
/*    struct Entry {
        value_type value;                       // sizeof(ValueType)
        key_type   key;                         // 4 or 8 bytes

        // meta
        uint32_t   gen = 0;                     // 4 bytes // ABA protection
        index_type next = NullIdx;              // 2 or 4 bytes
        index_type prev = NullIdx;              // 2 or 4 bytes
        slot_state state = slot_state::Empty;   // 1 byte
    };*/
    struct Entry {
        // Group 1: Metadata (Hot)
        std::atomic<uint32_t> gen{0};           // 4 bytes
        std::atomic<slot_state> state{slot_state::Empty};   // 1 byte

        // Group 2: Search (Hot)
        key_type key;                           // 8 bytes

        // Group 3: LRU Links (Warm)
        index_type next = NullIdx;                        // 4 bytes
        index_type prev = NullIdx;                        // 4 bytes

        // Group 4: Data (Cold/Warm)
        value_type value;                       // sizeof(value_type)
    };
public:
    const Entry& get_entry(index_type idx) const noexcept {
        return _table[idx];
    }

    Entry& get_entry_mutable(index_type idx) noexcept{
        return _table[idx];
    }

/*    static_assert((sizeof(Entry) & (CacheLine - 1)) == 0,
        "Entry crosses cache line boundary unoptimally!");
*/
private:
    static_assert(std::has_single_bit(Capacity), "Capacity must be power of 2");
    static constexpr std::size_t TableSize = Capacity * 2; // Load factor 0.5
    static_assert(TableSize == Capacity * 2, "Load factor must be 0.5");
    static constexpr std::size_t Mask = TableSize - 1;

private:

    struct LookupResult {
        ValueType* ptr;
        index_type idx;
        uint32_t   gen;
        bool       found;
    };

    std::size_t calculate_hash_idx(const KeyType& key) const noexcept {
        return std::hash<KeyType>{}(key) & Mask;
    }

    std::size_t next_slot(std::size_t current_idx) const noexcept {
        return (current_idx + 1) & Mask;
    }

    void detach(const index_type& idx) noexcept {
        Entry& current = _table[idx];

        if (current.next != NullIdx) _table[current.next].prev = current.prev;
        else _tail = current.prev;

        if (current.prev != NullIdx) _table[current.prev].next = current.next;
        else _head = current.next;

        current.next = NullIdx;
        current.prev = NullIdx;
    }

    void push_front(index_type idx) noexcept {
        auto& current = _table[idx];
        current.next = _head;
        current.prev = NullIdx;

        if (_head != NullIdx) { _table[_head].prev = idx; }
        _head = idx;

        if (_tail == NullIdx) _tail = idx;
    }

public:
    Lv2_LinkedFlatMap() noexcept : _table(std::make_unique<Entry[]>(TableSize)) {}

    bool is_occupied(index_type idx) const noexcept {
        return _table[idx].state == slot_state::Occupied;
    }

    bool is_valid_gen(index_type idx, uint32_t gen) const noexcept {
        return _table[idx].state == slot_state::Occupied && _table[idx].gen == gen;
    }

    std::size_t size() const noexcept { return _size; }
    index_type get_tail() const noexcept { return _tail; }
    index_type get_head() const noexcept { return _head; }

    LookupResult lookup(const KeyType& key) const noexcept {
        std::size_t idx = calculate_hash_idx(key);
        index_type first_del = NullIdx;

        while (true) {
            const auto& current = _table[idx];

            if (current.state == slot_state::Empty) {
                index_type target = (first_del != NullIdx) ? first_del : static_cast<index_type>(idx);
                return {nullptr, target, 0, false};
            }

            if (current.state == slot_state::Deleted && first_del == NullIdx) {
                first_del = static_cast<index_type>(idx);
            }

            if (current.state == slot_state::Occupied && current.key == key) {
                return {const_cast<ValueType*>(&current.value), static_cast<index_type>(idx), current.gen, true};
            }

            idx = next_slot(idx);
        }

        // It can loop for eternity only if Load Factor > 1.0 (all slots are Occupied or Deleted)
        // But since it has Capacity * 2 and there is no deletion of Empty slots, this is impossible.
        assert(false && "LinkedFlatMap table size overflow or corrupted logic");
        __builtin_unreachable(); // I'm sure
    }

    LookupResult get_lockless(const KeyType& key) const noexcept {
        std::size_t idx = calculate_hash_idx(key);

        for (std::size_t i = 0; i < TableSize; ++i) {
            const auto& current = _table[idx];

            uint32_t gen1 = current.gen.load(std::memory_order_acquire);
            slot_state current_state = current.state.load(std::memory_order_relaxed);

            if (current_state == slot_state::Empty) return {nullptr, NullIdx, 0, false};;

            if (current_state == slot_state::Occupied && current.key == key) {
                ValueType val = current.value;
                uint32_t gen2 = current.gen.load(std::memory_order_acquire); // MB on

                if (gen1 == gen2 && (gen1 % 2 == 0)) { // value is still actual
                    return {const_cast<ValueType*>(&current.value), static_cast<index_type>(idx), gen1, true};
                } else {
                    return {nullptr, NullIdx, 0, false};
                }
            }
            current = next_slot(idx);
        }

        return {nullptr, NullIdx, 0, false};;
    }

    template <typename... Args>
    void emplace_at(index_type idx, const KeyType& key, Args&&... args) noexcept {
    static_assert(std::is_nothrow_constructible_v<ValueType, Args...>,
                  "ValueType must be nothrow constructible for safety");

        auto& current = _table[idx];
        current.key = key;
        current.gen++;

        new (&current.value) value_type(std::forward<Args>(args)...); // Memory had been allocated by assign_slot

        current.state = slot_state::Occupied;
        current.gen++;
        _size++;
    }

    index_type assign_slot(const key_type& key) noexcept {
        std::size_t idx = calculate_hash_idx(key);
        index_type first_del_idx_wth_same_key = NullIdx;

        while (true) {
            if (_table[idx].state == slot_state::Empty) {
                return (first_del_idx_wth_same_key != NullIdx) ? first_del_idx_wth_same_key
                                                                             : static_cast<index_type>(idx);
            }

            if (_table[idx].state == slot_state::Deleted && first_del_idx_wth_same_key == NullIdx) {
                first_del_idx_wth_same_key = static_cast<index_type>(idx);
            }

            idx = next_slot(idx);
        }

        assert(false && "LinkedFlatMap table size overflow or corrupted logic");
        __builtin_unreachable(); // I'm sure
    }

    void move_to_front(index_type idx) noexcept {
        if (idx == _head || idx == NullIdx) return;

        Entry& curr = _table[idx];
        if (curr.next != NullIdx) __builtin_prefetch(&_table[curr.next], 1, 3);
        if (curr.prev != NullIdx) __builtin_prefetch(&_table[curr.prev], 1, 3);

        detach(idx);

        push_front(idx);
    }

    void erase_index(const index_type& idx) noexcept {
        if (idx == NullIdx || _table[idx].state != slot_state::Occupied) return;

        detach(idx);

        _table[idx].value.~ValueType(); // Due to placement new
        _table[idx].state = slot_state::Deleted;
        _table[idx].gen++;
        _size--;
    }

private:
    std::unique_ptr<Entry[]> _table;
    index_type _head = NullIdx;
    index_type _tail = NullIdx;
    std::size_t _size = 0;
};

/*
*   TODO:   Summary       Almost wait-free Read
*   TODO:                       uses Generation as Sequence Lock in FlatMap
*   TODO:                       std::atomic<slot_state>
*   TODO:                       lock-free get()
*   TODO:                       unique_lock for Writer
*/
template <Hashable KeyType, typename ValueType, std::size_t Capacity = 4 * 1024, std::size_t MaxThreads = 32>
requires PowerOfTwoValue<MaxThreads>
class Lv4_bdFlatLRU : private NonCopyableNonMoveable {
public:
    static constexpr const char* name() noexcept { return "Lv4_SPSCBuffer_DeferredFlatLRU"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:
    using cacheMap = Lv2_LinkedFlatMap<KeyType, ValueType, Capacity>;

    struct UpdateOp {
        cacheMap::index_type    idx;
        uint32_t                gen;
    };

    using SPSCBuffer = SPSC_RingBufferUltraFast<UpdateOp, Capacity / (4 * MaxThreads)>;

    static constexpr std::size_t CacheLine = 64; // Some hardcode :)
    static_assert(std::has_single_bit(MaxThreads), "MaxThreads must be a power of 2!");

private:
    struct alignas(CacheLine) PaddedSPSC : public SPSCBuffer {
        char padding[CacheLine - (sizeof(SPSCBuffer) & (CacheLine - 1))];
    };

private:

    static std::size_t get_thread_id() {
        static std::atomic<std::size_t> counter{0};

        // rollcall
        thread_local std::size_t id = std::numeric_limits<std::size_t>::max();

        if (id == std::numeric_limits<std::size_t>::max()) [[unlikely]] {
            id = counter.fetch_add(1, std::memory_order_relaxed) & (MaxThreads - 1);
        }

        return id;
    }

    void apply_updates() {
        uint64_t mask = _dirty_mask.exchange(0, std::memory_order_acquire);
        if (mask == 0) return;

        // Iterate only for set bits
        while (mask > 0) {
            int buf_idx = std::countr_zero(mask); // Index by set bit
            UpdateOp op;

            while (_update_buffers[buf_idx].pop(op)) {
                if (_collection.is_valid_gen(op.idx, op.gen)) {
                    __builtin_prefetch(&_collection.get_entry(_collection.get_head()), 1, 3);
                    _collection.move_to_front(op.idx);
                }
            }

            mask &= (mask - 1); // Next bit
        }
    }

public:
    std::optional<ValueType> get(const KeyType& key) noexcept {
        auto res = _collection.lookup(key);
        if (!res.found) [[unlikely]] return {};

//        __builtin_prefetch(&_collection.get_entry(res.idx), 1, 3);

        auto tid = get_thread_id();
        if (_update_buffers[tid].push({res.idx, res.gen})) {
            if (!(_dirty_mask.load(std::memory_order_relaxed) & (1ULL << tid))) {   // Test
                _dirty_mask.fetch_or(1ULL << tid, std::memory_order_release);       // Test & Set bit in mask
            }
        }

        return *(res.ptr);
    }

    template <typename T>
    void put(const KeyType& key, T&& value) {
        std::unique_lock lock(_rw_mtx);

        if (_dirty_mask.load(std::memory_order_relaxed)) {
            apply_updates();
        }

        auto res = _collection.lookup(key);

        if (res.found) {
            auto& entry = _collection.get_entry_mutable(res.idx);

            uint32_t current_gen = entry.gen.load(std::memory_order_relaxed);
            entry.gen.store(current_gen + 1, std::memory_order_release);
            *(res.ptr) = std::forward<T>(value);

            entry.gen.store(current_gen + 2, std::memory_order_release);
            _collection.move_to_front(res.idx);
        } else {
            if (_collection.size() >= Capacity) {
                _collection.erase_index(_collection.get_tail());
                res.idx = _collection.assign_slot(key);
            }

            _collection.emplace_at(res.idx, key, std::forward<T>(value));
            _collection.move_to_front(res.idx);
        }
    }

private:
    alignas(CacheLine) PaddedSPSC               _update_buffers[MaxThreads];
    alignas(CacheLine) std::atomic<uint64_t>    _dirty_mask{0};

    cacheMap            _collection;
    std::shared_mutex   _rw_mtx;
};
