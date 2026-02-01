#include <iostream>
#include <memory>

template <typename T, typename Allocator = std::allocator<T>>
class LoggingAllocator : public Allocator {
public:
    using value_type = T;

    T* allocate(std::size_t n) {
        std::cout << "Allocating " << n << " objects of type " << typeid(T).name() << std::endl;
        return Allocator::allocate(n);
    }

    void deallocate(T* p, std::size_t n) {
        std::cout << "Deallocating " << n << " objects of type " << typeid(T).name() << std::endl;
        Allocator::deallocate(p, n);
    }
};
