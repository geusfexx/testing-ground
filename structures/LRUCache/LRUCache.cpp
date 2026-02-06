#include <optional>
#include <unordered_map>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <memory>

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

    void put(const KeyType& key, ValueType value) {
        std::lock_guard<std::mutex> lock(_mtx);
        auto it = _collection.find(key);
        if (it != _collection.end()) {
            it->second->second = std::move(value);
            refresh(it);
        } else {
            if (_freq_list.size() == Capacity) {
                _collection.erase(_freq_list.back().first);
                _freq_list.pop_back();
            }
            _freq_list.emplace_front(key, std::move(value));
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

    void put(const KeyType& key, ValueType value) {
        _lock.lock();
        auto it = _collection.find(key);
        if (it != _collection.end()) {
            it->second->second = std::move(value);
            refresh(it);
        } else {
            if (_freq_list.size() == Capacity) {
                _collection.erase(_freq_list.back().first);
                _freq_list.pop_back();
            }
            _freq_list.emplace_front(key, std::move(value));
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
template <typename T, std::size_t Capacity>
class MPSC_TraceBuffer : private NonCopyableNonMoveable{    // Use EBO
public:
    static constexpr const char* name() noexcept { return "MPSC_TraceBuffer"; }

private:
    static constexpr std::size_t CacheLine = 64; // Some hardcode :)
    static constexpr bool isPowerOfTwo(std::size_t n) { return (n != 0) && (n & (n - 1)) == 0; }
    static constexpr std::size_t Mask = Capacity - 1;
    static_assert(isPowerOfTwo(Capacity), "Capacity must be power of 2");

public:
    bool isItTime() const noexcept {
        return (tail - head > (Capacity / 2));
    }

    bool push(const T& value) {
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

    bool pop(T& value) {
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
    T buffer[Capacity];

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

    void put(const KeyType& key, ValueType value) {
        std::unique_lock<std::shared_mutex> lock(_rw_mtx);

        if (_update_buffer.isItTime()) {
            apply_updates(); // Apply cummulative updates by writers
        }

        auto it = _collection.find(key);
        if (it != _collection.end()) {
            it->second->second = std::move(value);
        } else {
            if (_freq_list.size() == Capacity) {
                apply_updates(); // Emergency apply

                _collection.erase(_freq_list.back().first);
                _freq_list.pop_back();
            }
            _freq_list.emplace_front(key, std::move(value));
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
class LinearFlatMap : private NonCopyableNonMoveable { // Open Addressing table with Linear Probing
    struct Entry {
        KeyType key;
        ValueType value;
        bool occupied = false;
        bool deleted = false;   // Tombstone
    };

public:
    static constexpr const char* name() noexcept { return "LinearFlatMap"; }

private:
    static constexpr bool isPowerOfTwo(std::size_t n) { return (n != 0) && (n & (n - 1)) == 0; }
    static_assert(isPowerOfTwo(Capacity), "Capacity must be power of 2");
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
    DeferredFlatLRU() { }

    std::optional<ValueType> get(const KeyType& key) noexcept {
        std::shared_lock lock(_rw_mtx);

        auto it = _collection.find(key);
        if (!it) return {};

        // Admission of losses
        // Accumulate updates
        _update_buffer.push(key);

        return (*it)->second;
    }

    void put(const KeyType& key, ValueType value) {
        std::unique_lock<std::shared_mutex> lock(_rw_mtx);

        if (_update_buffer.isItTime()) {
            apply_updates(); // Apply cummulative updates by writers
        }

        auto it = _collection.find(key);
        if (it) {
            (*it)->second = std::move(value);
        } else {
            if (_freq_list.size() == Capacity) {
                apply_updates(); // Emergency apply

                _collection.erase(_freq_list.back().first);
                _freq_list.pop_back();
            }
            _freq_list.emplace_front(key, std::move(value));
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
    std::size_t TotalCapacity = 1024,
    std::size_t ShardsCount = 16>
class ShardedCache : private NonCopyableNonMoveable {
public:
    static constexpr std::string name() noexcept {
        return "Sharded<" + std::string(Cache::name()) + ">";
    }

private:
    static constexpr std::size_t Mask = ShardsCount - 1;
    static constexpr bool isPowerOfTwo(std::size_t n) { return (n != 0) && (n & (n - 1)) == 0; }
    static_assert(TotalCapacity > 0, "TotalCapacity must be > 0");
    static_assert(ShardsCount > 0, "ShardsCount must be > 0");
    static_assert(isPowerOfTwo(ShardsCount), "ShardsCount must be power of 2");

    using Cache = CacheImpl<KeyType, ValueType, TotalCapacity / ShardsCount>;

    std::size_t get_shard_idx(const KeyType& key) const noexcept {
        return std::hash<KeyType>{}(key) & Mask;
    }

public:
    ShardedCache() : _shards(ShardsCount) {
        for (auto& shard : _shards) {
            shard = std::make_unique<Cache>();
        }
    }

    std::optional<ValueType> get(const KeyType& key) noexcept {
        return _shards[get_shard_idx(key)]->get(key);
    }

    void put(const KeyType& key, ValueType value) {
        _shards[get_shard_idx(key)]->put(key, std::move(value));
    }

private:
    std::vector<std::unique_ptr<Cache>> _shards;
};
