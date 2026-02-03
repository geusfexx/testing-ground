#include <iostream>
#include "LRUCache.cpp"

int main()
{
    LRUCache<int, double, 2> lru(2);

    lru.put(1, 1.1);
    lru.put(2, 2.5);

    std::cout << lru.get(2).value_or(-1) << std::endl;
    lru.put(3, 3.33);
    std::cout << lru.get(1).value_or(-1) << std::endl;
    
    return 0;
}
