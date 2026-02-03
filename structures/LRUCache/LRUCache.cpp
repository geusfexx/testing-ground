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
    explicit LRUCache() {_collection.reserve(Capacity);}

    std::optional<ValueType> get(const KeyType& key) noexcept {
        auto it = _collection.find(key);
        if (it == _collection.end()) return {};
        refresh(it);
        return it->second->second;
    }

    void put(const KeyType& key, ValueType value) {
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
    cacheList   _freq_list;         // key, value
    cacheMap    _collection;        // key, cacheList::iterator
};

