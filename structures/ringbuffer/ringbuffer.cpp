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
