#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include "LRUCache.cpp"

const int iterations = 1e6;
const int key_range = 2000;
    
template<typename Cache, int Readers, int Writers>
void run_test(Cache& cache, long long iterations) {
    auto start = std::chrono::high_resolution_clock::now();
    int misses = 0;

    std::vector<std::thread> threads;

    auto reader = [&]() {
        std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<> dist(0, key_range);
        for (long long i = 0; i < iterations; ++i) {
            if (!cache.get(dist(gen))) ++misses; // Very approximately :)
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

    for (int i = 0; i < Readers; ++i) threads.emplace_back(reader);
    for (int i = 0; i < Writers; ++i) threads.emplace_back(writer);

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Time: " << diff.count() << " s \nOps/sec: "
              << ((Readers + Writers ) * iterations / diff.count()) / 1e6 << " M\n"
              << "Misses: " << misses << "\n\n";
}

int main() {

    {
        const int reader_count = 4;
        const int writer_count = 2;
        const int cache_size = 1024;

        std::cout << "Testing scenario: readers - " << reader_count << " writers - " << writer_count << std::endl << std::endl;
        {
            std::cout << "Testing LRUCacheSlow..." << std::endl;
            LRUCacheSlow<int, int, cache_size> cache;
            run_test<decltype(cache), reader_count, writer_count>(cache, iterations);
        }

        {
            std::cout << "Testing LRUCacheSpin..." << std::endl;
            LRUCacheSpin<int, int, cache_size> cache;
            run_test<decltype(cache), reader_count, writer_count>(cache, iterations);
        }

        {
            std::cout << "Testing LRUCacheAccumulative..." << std::endl;
            LRUCacheAccumulative<int, int, cache_size> cache;
            run_test<decltype(cache), reader_count, writer_count>(cache, iterations);
        }

        std::cout << "Done: " << (reader_count + writer_count) << " threads finished." << std::endl << std::endl;
    }

    {
        const int reader_count = 2;
        const int writer_count = 12;
        const int cache_size = 1024;

        std::cout << "Testing scenario: readers - " << reader_count << " writers - " << writer_count << std::endl << std::endl;
        {
            std::cout << "Testing LRUCacheSlow..." << std::endl;
            LRUCacheSlow<int, int, cache_size> cache;
            run_test<decltype(cache), reader_count, writer_count>(cache, iterations);
        }

        {
            std::cout << "Testing LRUCacheSpin..." << std::endl;
            LRUCacheSpin<int, int, cache_size> cache;
            run_test<decltype(cache), reader_count, writer_count>(cache, iterations);
        }

        {
            std::cout << "Testing LRUCacheAccumulative..." << std::endl;
            LRUCacheAccumulative<int, int, cache_size> cache;
            run_test<decltype(cache), reader_count, writer_count>(cache, iterations);
        }

        std::cout << "Done: " << (reader_count + writer_count) << " threads finished." << std::endl << std::endl;
    }

    {
        const int reader_count = 24;
        const int writer_count = 4;
        const int cache_size = 1024;

        std::cout << "Testing scenario: readers - " << reader_count << " writers - " << writer_count << std::endl << std::endl;
        {
            std::cout << "Testing LRUCacheSlow..." << std::endl;
            LRUCacheSlow<int, int, cache_size> cache;
            run_test<decltype(cache), reader_count, writer_count>(cache, iterations);
        }

        {
            std::cout << "Testing LRUCacheSpin..." << std::endl;
            LRUCacheSpin<int, int, cache_size> cache;
            run_test<decltype(cache), reader_count, writer_count>(cache, iterations);
        }

        {
            std::cout << "Testing LRUCacheAccumulative..." << std::endl;
            LRUCacheAccumulative<int, int, cache_size> cache;
            run_test<decltype(cache), reader_count, writer_count>(cache, iterations);
        }

        std::cout << "Done: " << (reader_count + writer_count) << " threads finished." << std::endl << std::endl;
    }

    return 0;
}
