#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <array>
#include "LRUCache.cpp"

struct TestConfig {
    int readers;
    int writers;
    int cache_size;
    int key_range;
    int key_amount;
    long long iterations;
};

template<std::size_t Size>
struct alignas(64) Payload {
    static_assert(Size >= sizeof(uint64_t), "Size must be at least 8 bytes");
    static constexpr std::size_t PaddingSize = Size - sizeof(uint64_t);

    uint64_t id;
    std::array<std::byte, PaddingSize> data;

    Payload(uint64_t v = 0) : id(v) {
        if constexpr (PaddingSize > 0) {
            // Avoid "empty object" optimization by the compiler
            data.fill(std::byte(v & 0xFF));
        }
    }

    bool operator==(const Payload& other) const { return id == other.id; }
};

namespace std {
    template<std::size_t Size>
    struct hash<Payload<Size>> {
        std::size_t operator()(const Payload<Size>& p) const {
            return std::hash<uint64_t>{}(p.id);
        }
    };
}


template<typename Cache, bool UseYield = false>
void run_benchmark(const TestConfig& config) {
    Cache cache;
    std::atomic<long long> total_misses{0};
    std::vector<std::thread> threads;
    std::vector<int> keys(100'000);

    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dist(0, config.key_range);

    for(auto& k : keys) k = dist(gen);

    std::cout << "Testing: " << Cache::name() << (UseYield ? " (with yield)" : "") << "..." << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < config.readers; ++i) {
        threads.emplace_back([&cache, &config, &total_misses, &keys, i]() {

            int local_misses = 0;

            for (long long j = 0; j < config.iterations; ++j) {
                if (!cache.get(keys[j & (config.key_amount - 1)])) local_misses++;
                if constexpr (UseYield) std::this_thread::yield();
            }
            total_misses.fetch_add(local_misses, std::memory_order_relaxed);
        });
    }

    for (int i = 0; i < config.writers; ++i) {
        threads.emplace_back([&cache, &config, &keys]() {
            typename Cache::value_type val{42};
            for (long long j = 0; j < config.iterations; ++j) {
                cache.put((keys[j & (config.key_amount - 1)]), val);
                if constexpr (UseYield) std::this_thread::yield();
            }
        });
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    //long long total_misses = 0;
    //for (int m : thread_misses) total_misses += m;
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
    const long long iters = 1e6;
    const int cache_sz = 4 * 1024;
    const int k_range = (cache_sz * 12) / 10;
    const int key_amount = 100'000;

    TestConfig read_heavy  = {28, 4, cache_sz, k_range, key_amount, iters};
    TestConfig write_heavy = {4, 12, cache_sz, k_range, key_amount, iters};
    TestConfig balanced    = {4, 2, cache_sz, k_range, key_amount, iters};


    using Slow = StrictLRU<int, Payload<128>, cache_sz>;
    using Spin = SpinlockedLRU<int, Payload<128>, cache_sz>;
    using Def  = DeferredLRU<int, Payload<128>, cache_sz>;
    using DefFM = DeferredFlatLRU<int, Payload<128>, cache_sz>;
    using SPSCBDefFM = SPSCBuffer_DeferredFlatLRU<int, Payload<128>, cache_sz>;
    using Lv2_SPSCBDefFM = Lv2_SPSCBuffer_DeferredFlatLRU<int, Payload<128>, cache_sz>;

    using S_Slow = ShardedCache<StrictLRU, int, Payload<128>, cache_sz, 32>;
    using S_Spin = ShardedCache<SpinlockedLRU, int, Payload<128>, cache_sz, 32>;
    using S_Def  = ShardedCache<DeferredLRU, int, Payload<128>, cache_sz, 32>;
    using S_DefFM = ShardedCache<DeferredFlatLRU, int, Payload<128>, cache_sz, 32>;
    using S_SPSCBDefFM = ShardedCache<SPSCBuffer_DeferredFlatLRU, int, Payload<128>, cache_sz, 32>;
    using S_Lv2_SPSCBDefFM = ShardedCache<Lv2_SPSCBuffer_DeferredFlatLRU, int, Payload<128>, cache_sz, 32>;

//    execute_scenario<false, Slow, Spin, Def, DefFM, SPSCBDefFM, S_Slow, S_Spin, S_Def, S_DefFM, S_SPSCBDefFM>(balanced);
//    execute_scenario<false, Slow, Spin, Def, DefFM, SPSCBDefFM, S_Slow, S_Spin, S_Def, S_DefFM, S_SPSCBDefFM>(write_heavy);
    execute_scenario<false,/* Slow, Spin, Def, DefFM, SPSCBDefFM, Lv2_SPSCBDefFM, S_Slow, S_Spin, */S_Def, S_DefFM, S_SPSCBDefFM, S_Lv2_SPSCBDefFM>(read_heavy);
/*
    execute_scenario<true, Slow, Spin, Def, DefFM, SPSCBDefFM, S_Slow, S_Spin, S_Def, S_DefFM, S_SPSCBDefFM>(balanced);
    execute_scenario<true, Slow, Spin, Def, DefFM, SPSCBDefFM, S_Slow, S_Spin, S_Def, S_DefFM, S_SPSCBDefFM>(write_heavy);
    execute_scenario<true, Slow, Spin, Def, DefFM, SPSCBDefFM, S_Slow, S_Spin, S_Def, S_DefFM, S_SPSCBDefFM>(read_heavy);
*/
    return 0;
}
