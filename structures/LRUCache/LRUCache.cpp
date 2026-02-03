#include <optional>
#include <unordered_map>
#include <list>
#include <mutex>

template <typename KeyType, typename ValueType, std::size_t Capacity = 1024>
class LRUCacheSlow {

    static_assert(Capacity > 0);

    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = std::unordered_map<KeyType, typename cacheList::iterator>;
    
    void refresh(typename cacheMap::iterator it) {
        _freq_list.splice(_freq_list.begin(), _freq_list, it->second);
    }

public:
    explicit LRUCacheSlow() { _collection.reserve(Capacity); }

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
class LRUCacheSpin {

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
    explicit LRUCacheSpin() { _collection.reserve(Capacity); }

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
class SPSC_RingBufferUltraFast {
    
    static constexpr std::size_t CacheLine = 64; // Some hardcode :)
    static constexpr bool isPowerOfTwo(std::size_t n) { return (n != 0) && (n & (n - 1)) == 0; }

    std::size_t increment(std::size_t i) const noexcept {
        if constexpr (isPowerOfTwo(Capacity)) return (i + 1) & (Capacity - 1);
        else return (i + 1) % Capacity;
    }

public:
    bool push(const T& value) {
        const std::size_t curr_t = tail.load(std::memory_order_relaxed);

        if (increment(curr_t) == head_cache) {
            head_cache = head.load(std::memory_order_acquire); // MB on
            if (increment(curr_t) == head_cache) return false;
        }

        buffer[curr_t] = value;
        tail.store(increment(curr_t), std::memory_order_release); // MB off

        return true;
    }

    bool pop(T& value) {
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
    T buffer[Capacity];

    // Producer's group
    alignas(CacheLine) std::atomic<std::size_t> tail{0};
    std::size_t head_cache{0}; // local

    // Consumer's group
    alignas(CacheLine) std::atomic<std::size_t> head{0};
    std::size_t tail_cache{0}; // local
};

// FIXME    free(): invalid pointer // It needs to avoid segfault by invalidating iterators
// TODO     Fix by ordinary mutex, by MPMC RBuffer or ...

template <typename KeyType, typename ValueType, std::size_t Capacity = 1024>
class LRUCacheAccumulative {

    static_assert(Capacity > 0);

    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = std::unordered_map<KeyType, typename cacheList::iterator>;
    using ringBuffer = SPSC_RingBufferUltraFast<KeyType, 512>;

private:    
    void apply_updates() {

        KeyType key_to_refresh;
        while (_update_buffer.pop(key_to_refresh)) {
            auto it = _collection.find(key_to_refresh);
            if (it != _collection.end()) {
                _freq_list.splice(_freq_list.begin(), _freq_list, it->second);
            }
        }
    }

public:
    explicit LRUCacheAccumulative() { _collection.reserve(Capacity); }

    std::optional<ValueType> get(const KeyType& key) noexcept {
        auto it = _collection.find(key);
        if (it == _collection.end()) return {};

        // Admission of losses
        // Accumulate updates
        _update_buffer.push(key);

        return it->second->second;
    }

    void put(const KeyType& key, ValueType value) {
        std::unique_lock<std::mutex> lock(_map_mtx); // Hotfix

        apply_updates(); // Apply cummulative updates by writers

        auto it = _collection.find(key);
        if (it != _collection.end()) {
            it->second->second = std::move(value);
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
    ringBuffer  _update_buffer;
    cacheList   _freq_list;         // key, value
    cacheMap    _collection;        // key, cacheList::iterator
    std::mutex  _map_mtx;           // Hotfix
};

