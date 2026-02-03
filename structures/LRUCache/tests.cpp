#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include "LRUCache.cpp"

const int iterations = 1e5;
const int key_range = 200;
const int reader_count = 4;
const int writer_count = 2;
    
template<typename Cache>
void run_test(Cache& cache, long long iterations) {
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;

    auto reader = [&]() {
        std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<> dist(0, key_range);
        for (long long i = 0; i < iterations; ++i) {
            cache.get(dist(gen));
            //std::this_thread::yield(); // turning the rules upside down
        }
    };

    auto writer = [&]() {
        std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<> dist(0, key_range);
        for (long long i = 0; i < iterations; ++i) {
            cache.put(dist(gen), dist(gen));
            //std::this_thread::yield(); // turning the rules upside down
        }
    };

    for (int i = 0; i < reader_count; ++i) threads.emplace_back(reader);
    for (int i = 0; i < writer_count; ++i) threads.emplace_back(writer);

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Time: " << diff.count() << " s \nOps/sec: " << ((reader_count + writer_count ) * iterations / diff.count()) / 1e6 << " M\n\n";
}

int main() {

    {
        std::cout << "Testing LRUCacheSlow..." << std::endl;
        LRUCacheSlow<int, int, 100> cache;
        run_test(cache, iterations);
    }
    
    {
        std::cout << "Testing LRUCacheSpin..." << std::endl;
        LRUCacheSpin<int, int, 100> cache;
        run_test(cache, iterations);
    }

    std::cout << "Done: " << (reader_count + writer_count) << " threads finished." << std::endl;

    return 0;
}
