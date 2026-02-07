#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include "LRUCache.cpp"

struct TestConfig {
    int readers;
    int writers;
    int cache_size;
    int key_range;
    long long iterations;
};

template<typename Cache, bool UseYield = false>
void run_benchmark(const TestConfig& config) {
    Cache cache;
    std::vector<int> thread_misses(config.readers, 0);
    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();

    std::cout << "Testing: " << Cache::name() << (UseYield ? " (with yield)" : "") << "..." << std::endl;

    for (int i = 0; i < config.readers; ++i) {
        threads.emplace_back([&cache, &config, &thread_misses, i]() {
            std::mt19937 gen(std::random_device{}());
            std::uniform_int_distribution<> dist(0, config.key_range);
            int local_misses = 0;

            for (long long j = 0; j < config.iterations; ++j) {
                if (!cache.get(dist(gen))) local_misses++;
                if constexpr (UseYield) std::this_thread::yield();
            }
            thread_misses[i] = local_misses;
        });
    }

    for (int i = 0; i < config.writers; ++i) {
        threads.emplace_back([&cache, &config]() {
            std::mt19937 gen(std::random_device{}());
            std::uniform_int_distribution<> dist(0, config.key_range);

            for (long long j = 0; j < config.iterations; ++j) {
                cache.put(dist(gen), dist(gen));
                if constexpr (UseYield) std::this_thread::yield();
            }
        });
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    long long total_misses = 0;
    for (int m : thread_misses) total_misses += m;
    double total_ops = (config.readers + config.writers) * config.iterations;

    std::cout << "Time: " << diff.count() << " s \n"
              << "Ops/sec: " << (total_ops / diff.count()) / 1e6 << " M\n"
              << "Misses: " << total_misses << "\n\n";
}

template<bool UseYield = false, typename... Caches>
void execute_scenario(const TestConfig& config) {
    std::cout << "================================================\n"
              << "SCENARIO: Readers(" << config.readers << ") Writers(" << config.writers << ")\n"
              << (UseYield ? "\t| YIELD MODE |" : "\t| NORMAL MODE |") << "\n"
              << "CacheSize: " << config.cache_size << " KeyRange: " << config.key_range << "\n"
              << "================================================\n" << std::endl;

    (run_benchmark<Caches, UseYield>(config), ...);

    std::cout << "Done: " << (config.readers + config.writers) << " threads finished.\n" << std::endl;
}

int main()
{
    const long long iters = 1e7;
    const int cache_sz = 1024;
    const int k_range = 1200;

    TestConfig read_heavy  = {24, 4, cache_sz, k_range, iters};
    TestConfig write_heavy = {4, 12, cache_sz, k_range, iters};
    TestConfig balanced    = {4, 2, cache_sz, k_range, iters};


    using Slow = StrictLRU<int, int, cache_sz>;
    using Spin = SpinlockedLRU<int, int, cache_sz>;
    using Def  = DeferredLRU<int, int, cache_sz>;
    using DefFM = DeferredFlatLRU<int, int, cache_sz>;
    using SPSCBDefFM = SPSCBuffer_DeferredFlatLRU<int, int, cache_sz>;
    using Lv2_SPSCBDefFM = Lv2_SPSCBuffer_DeferredFlatLRU<int, int, cache_sz>;

    using S_Slow = ShardedCache<StrictLRU, int, int, cache_sz, 16>;
    using S_Spin = ShardedCache<SpinlockedLRU, int, int, cache_sz, 16>;
    using S_Def  = ShardedCache<DeferredLRU, int, int, cache_sz, 16>;
    using S_DefFM = ShardedCache<DeferredFlatLRU, int, int, cache_sz, 16>;
    using S_SPSCBDefFM = ShardedCache<SPSCBuffer_DeferredFlatLRU, int, int, cache_sz, 16>;
    using S_Lv2_SPSCBDefFM = ShardedCache<Lv2_SPSCBuffer_DeferredFlatLRU, int, int, cache_sz, 16>;

//    execute_scenario<false, Slow, Spin, Def, DefFM, SPSCBDefFM, S_Slow, S_Spin, S_Def, S_DefFM, S_SPSCBDefFM>(balanced);
//    execute_scenario<false, Slow, Spin, Def, DefFM, SPSCBDefFM, S_Slow, S_Spin, S_Def, S_DefFM, S_SPSCBDefFM>(write_heavy);
    execute_scenario<false, /*Slow, Spin, Def, DefFM, SPSCBDefFM, Lv2_SPSCBDefFM, S_Slow, S_Spin, */S_Def, S_DefFM, S_SPSCBDefFM, S_Lv2_SPSCBDefFM>(read_heavy);
/*
    execute_scenario<true, Slow, Spin, Def, DefFM, SPSCBDefFM, S_Slow, S_Spin, S_Def, S_DefFM, S_SPSCBDefFM>(balanced);
    execute_scenario<true, Slow, Spin, Def, DefFM, SPSCBDefFM, S_Slow, S_Spin, S_Def, S_DefFM, S_SPSCBDefFM>(write_heavy);
    execute_scenario<true, Slow, Spin, Def, DefFM, SPSCBDefFM, S_Slow, S_Spin, S_Def, S_DefFM, S_SPSCBDefFM>(read_heavy);
*/
    return 0;
}
