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
class LRUCacheSlow : private NonCopyableNonMoveable{    // Use EBO

    static_assert(Capacity > 0);

    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = std::unordered_map<KeyType, typename cacheList::iterator>;
    
    void refresh(typename cacheMap::iterator it) {
        _freq_list.splice(_freq_list.begin(), _freq_list, it->second);
    }

public:
    LRUCacheSlow() { _collection.reserve(Capacity); }

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
class LRUCacheSpin : private NonCopyableNonMoveable{    // Use EBO

    static_assert(Capacity > 0);

    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = std::unordered_map<KeyType, typename cacheList::iterator>;

private:    
    struct SpinLock {
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
        void lock() { while (flag.test_and_set(std::memory_order_acquire)); } // Spinlock
        void unlock() { flag.clear(std::memory_order_release); }
    };

    void refresh(typename cacheMap::iterator it) {
        _freq_list.splice(_freq_list.begin(), _freq_list, it->second);
    }

public:
    LRUCacheSpin() { _collection.reserve(Capacity); }

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
class MPSC_RingBufferUltraFast : private NonCopyableNonMoveable{    // Use EBO
    
    static constexpr std::size_t CacheLine = 64; // Some hardcode :)
    static constexpr bool isPowerOfTwo(std::size_t n) { return (n != 0) && (n & (n - 1)) == 0; }
    static constexpr std::size_t Mask = Capacity - 1;
    static_assert(isPowerOfTwo(Capacity), "Capacity must be power of 2");

public:
    bool isItTime() {
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
// TODO     sharding    ******      Done
// TODO     using flat map
template <typename KeyType, typename ValueType, std::size_t Capacity = 1024>
class LRUCacheAccumulative : private NonCopyableNonMoveable{    // Use EBO

    static_assert(Capacity > 0);

    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = std::unordered_map<KeyType, typename cacheList::iterator>;
    using ringBuffer = MPSC_RingBufferUltraFast<KeyType, Capacity / 4>;

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
    LRUCacheAccumulative() { _collection.reserve(Capacity); }

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

//  Wrapper for SharedLRU
template <template<typename, typename, std::size_t> class CacheImpl,
    typename KeyType, typename ValueType,
    std::size_t TotalCapacity = 1024,
    std::size_t ShardsCount = 16>
class ShardedLRUCache : private NonCopyableNonMoveable {
    static constexpr std::size_t Mask = ShardsCount - 1;
    static constexpr bool isPowerOfTwo(std::size_t n) { return (n != 0) && (n & (n - 1)) == 0; }
    static_assert(TotalCapacity > 0, "TotalCapacity must be > 0");
    static_assert(ShardsCount > 0, "ShardsCount must be > 0");
    static_assert(isPowerOfTwo(ShardsCount), "ShardsCount must be power of 2");

    using lruCache = CacheImpl<KeyType, ValueType, TotalCapacity / ShardsCount>;

    std::size_t get_shard_idx(const KeyType& key) const {
        return std::hash<KeyType>{}(key) & Mask;
    }

public:
    ShardedLRUCache() : _shards(ShardsCount) {
        for (auto& shard : _shards) {
            shard = std::make_unique<lruCache>();
        }
    }

    std::optional<ValueType> get(const KeyType& key) noexcept {
        return _shards[get_shard_idx(key)]->get(key);
    }

    void put(const KeyType& key, ValueType value) {
        _shards[get_shard_idx(key)]->put(key, std::move(value));
    }

private:
    std::vector<std::unique_ptr<lruCache>> _shards;
};
