#include <iostream>
#include <chrono>
#include <thread>
#include "ringbuffer.cpp"

template<typename Buffer>
void run_test(Buffer& pool, long long iterations) {
    auto start = std::chrono::high_resolution_clock::now();

    std::thread producer([&]() {
        for (long long i = 0; i < iterations; ++i) {
            while (!pool.push(i)) // std::this_thread::yield() // turning the rules upside down
            ;
        }
    });

    std::thread consumer([&]() {
        long long val;
        for (long long i = 0; i < iterations; ++i) {
            while (!pool.pop(val)) // std::this_thread::yield() // turning the rules upside down
            ;
        }
    });

    producer.join();
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    std::cout << "Time: " << diff.count() << " s \nOps/sec: " << (iterations / diff.count()) / 1e6 << " M\n";
}

int main() {
    const long long iterations = 1e8;
    const std::size_t capacity = 4 * 1024;

    std::cout << "Testing UltraFastSPSC RingBuffer..." << std::endl;
    SPSC_RingBufferUltraFast<long long, capacity> ultrafast;
    run_test(ultrafast, iterations);

    std::cout << "\nTesting FastSPSC RingBuffer..." << std::endl;
    SPSC_RingBufferFast<long long, capacity> fast;
    run_test(fast, iterations);

    std::cout << "\nTesting SlowSPSC RingBuffer..." << std::endl;
    SPSC_RingBufferSlow<long long> slow(capacity);
    run_test(slow, iterations);

    return 0;
}
