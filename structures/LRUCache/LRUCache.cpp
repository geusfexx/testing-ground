#include <optional>
#include <unordered_map>
#include <list>

template <typename KeyType, typename ValueType, std::size_t Capacity = 1024>
class LRUCache {

    static_assert(Capacity > 0);
    using cacheList = std::list<std::pair<KeyType, ValueType>>;
    using cacheMap = std::unordered_map<KeyType, typename cacheList::iterator>;
    
    void refresh(typename cacheMap::iterator it) {
        _freq_list.splice(_freq_list.begin(), _freq_list, it->second);
    }

public:
    LRUCache(size_t cap) {}

    std::optional<ValueType> get(KeyType key) {
        auto it = _collection.find(key);
        if (it == _collection.end()) return {};
        refresh(it);
        return it->second->second;
    }

    void put(KeyType key, ValueType value) {
        auto it = _collection.find(key);
        if (it != _collection.end()) {
            it->second->second = value;
            refresh(it);
        } else {
            if (_freq_list.size() == Capacity) {
                _collection.erase(_freq_list.back().first);
                _freq_list.pop_back();
            }
            _freq_list.emplace_front(key, value);
            _collection[key] = _freq_list.begin();
        }
    }

private:
    cacheList   _freq_list;         // key, value
    cacheMap    _collection;        // key, cacheList::iterator
};

