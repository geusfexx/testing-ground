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
