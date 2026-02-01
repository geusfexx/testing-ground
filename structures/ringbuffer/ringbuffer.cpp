#include <atomic>
#include <vector>

/* It has:  False Sharing problem
*           fixed size vector
*           acq-rel fence
*           ordinary division
*/
template <typename T>
class SPSC_RingBufferSlow {
public:
    SPSC_RingBufferSlow(std::size_t cap) : buffer(cap), capacity(cap) {}

    bool push(T val) {
        std::size_t curr_t = tail.load(std::memory_order_relaxed);
        std::size_t next = (curr_t + 1) % capacity;

        if (next == head.load(std::memory_order_acquire)) return false; // MB on
        buffer[curr_t] = val;
        tail.store(next, std::memory_order_release); // MB off

        return true;
    }

    bool pop(T& val) {
        std::size_t curr_h = head.load(std::memory_order_relaxed);

        if (curr_h == tail.load(std::memory_order_acquire)) return false; // MB on
        val = buffer[curr_h];
        head.store((curr_h + 1) % capacity, std::memory_order_release); // MB off

        return true;
    }

private:
    std::vector<T> buffer;
    std::size_t capacity;
    std::atomic<std::size_t> head{0};
    std::atomic<std::size_t> tail{0};
};

/* It has:  False Sharing resolved with alignas
*           array
*           acq-rel fence
*           division using bitwise "AND"
*/
template <typename T, std::size_t Capacity>
class SPSC_RingBufferFast {

    static constexpr std::size_t CacheLine = 64; // Some hardcode :)
    static constexpr bool isPowerOfTwo(std::size_t n) { return (n != 0) && (n & (n - 1)) == 0; }

    std::size_t increment(std::size_t i) const noexcept {
        if constexpr (isPowerOfTwo(Capacity)) return (i + 1) & (Capacity - 1);
        else return (i + 1) % Capacity;
    }

public:
    bool push(T val) {
        std::size_t curr_t = tail.load(std::memory_order_relaxed);
        std::size_t next = increment(curr_t);

        if (next == head.load(std::memory_order_acquire)) return false;
        buffer[curr_t] = val;
        tail.store(next, std::memory_order_release);

        return true;
    }

    bool pop(T& val) {
        std::size_t curr_h = head.load(std::memory_order_relaxed);

        if (curr_h == tail.load(std::memory_order_acquire)) return false;
        val = buffer[curr_h];
        head.store(increment(curr_h), std::memory_order_release);

        return true;
    }

private:
    T buffer[Capacity];
    alignas(CacheLine) std::atomic<std::size_t> head{0};
    alignas(CacheLine) std::atomic<std::size_t> tail{0};
};
