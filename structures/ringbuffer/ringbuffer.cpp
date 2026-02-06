#include <atomic>
#include <vector>

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

        if (next == head.load(std::memory_order_acquire)) return false; // MB on
        buffer[curr_t] = val;
        tail.store(next, std::memory_order_release); // MB off

        return true;
    }

    bool pop(T& val) {
        std::size_t curr_h = head.load(std::memory_order_relaxed);

        if (curr_h == tail.load(std::memory_order_acquire)) return false; // MB on
        val = buffer[curr_h];
        head.store(increment(curr_h), std::memory_order_release); // MB off

        return true;
    }

private:
    T buffer[Capacity];
    alignas(CacheLine) std::atomic<std::size_t> head{0};
    alignas(CacheLine) std::atomic<std::size_t> tail{0};
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

/* It has:  False Sharing resolved with alignas
*           array
*           acq-rel fence
*           division using bitwise "AND"
*           fetch_add instead of store
*/
template <typename T, std::size_t Capacity>
class SPSC_RingBufferExperimental {

    static constexpr std::size_t CacheLine = 64; // Some hardcode :)
    static constexpr bool isPowerOfTwo(std::size_t n) { return (n != 0) && (n & (n - 1)) == 0; }
    static constexpr std::size_t Mask = Capacity - 1;

    std::size_t constexpr increment(std::size_t i) const noexcept {
        if constexpr (isPowerOfTwo(Capacity)) return (i + 1) & Mask;
        else return (i + 1) % Capacity;
    }

public:
    bool push(const T& value) {
        const std::size_t curr_t = tail.load(std::memory_order_relaxed);

        if (curr_t - head_cache >= Capacity) {
            head_cache = head.load(std::memory_order_acquire); // MB on
            if (curr_t - head_cache >= Capacity) return false;
        }

        buffer[curr_t & Mask] = value;
        tail.fetch_add(1, std::memory_order_release); // MB off

        return true;
    }

    bool pop(T& value) {
        const std::size_t curr_h = head.load(std::memory_order_relaxed);

        if (curr_h == tail_cache) {
            tail_cache = tail.load(std::memory_order_acquire); // MB on
            if (curr_h == tail_cache) return false;
        }

        value = buffer[curr_h & Mask];
        head.fetch_add(1, std::memory_order_release); // MB off

        return true;
    }

private:
/*    static constexpr std::size_t get_index(size_t i) noexcept {
        return i & Mask;
    }
*/
    alignas(CacheLine) T buffer[Capacity];

    // Producer's group
    alignas(CacheLine) std::atomic<std::size_t> tail{0};
    std::size_t head_cache{0}; 

    // Consumer's group
    alignas(CacheLine) std::atomic<std::size_t> head{0};
    std::size_t tail_cache{0};
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
