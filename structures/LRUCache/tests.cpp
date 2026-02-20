#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <array>
#include <iomanip>
#include "LRUCache.cpp"
//#include "Lv6_bdFlatLRU.cpp"

struct TestConfig {
    int readers;
    int writers;
    int cache_size;
    int key_range;
    int key_amount;
    long long iterations;
    int payload_size = 128;
    int shards_amount = 32;
};

constexpr int key_amount = 10'000'000;

template<std::size_t Size>
struct alignas(64) Payload {
    static_assert(Size >= sizeof(uint64_t), "Size must be at least 8 bytes");
    static constexpr std::size_t PaddingSize = Size - sizeof(uint64_t);

    uint64_t id;
    std::array<std::byte, PaddingSize> data;

    Payload(uint64_t v = 0) noexcept : id(v) {
        if constexpr (PaddingSize > 0) {
            // Avoid "empty object" optimization by the compiler
            data.fill(std::byte(v & 0xFF));
        }
    }

    bool operator==(const Payload& other) const noexcept { return id == other.id; }

    ~Payload() noexcept {}
};

namespace std {
    template<std::size_t Size>
    struct hash<Payload<Size>> {
        std::size_t operator()(const Payload<Size>& p) const {
            return std::hash<uint64_t>{}(p.id);
        }
    };
}

template<std::size_t KeyAmount = 100'000>
class BenchmarkData {
public:
    std::vector<int> keys;

    static const BenchmarkData& get(int key_range) {
        static BenchmarkData instance(key_range);
        return instance;
    }

private:
    BenchmarkData(int key_range) {
        keys.resize(KeyAmount);
        // Equality & Deterministic data
        std::mt19937 gen(42);
        std::uniform_int_distribution<> dist(0, key_range);

        for (auto& k : keys) {
            k = dist(gen);
        }
    }
};

std::string format_large_num(double num) {
    const char* units[] = {"", " k", " M", " B", " T"};
    int unit_idx = 0;
    while (std::abs(num) >= 1000.0 && unit_idx < 4) {
        num /= 1000.0;
        unit_idx++;
    }
    std::stringstream ss;
    ss << std::fixed << std::setprecision(3) << num << units[unit_idx];
    return ss.str();
}

template<typename Cache, bool UseYield = false>
void run_benchmark(const TestConfig& config) {
    Cache cache;
    std::atomic<unsigned long long> total_misses{0};
    std::vector<std::thread> threads;
    const auto& data = BenchmarkData<key_amount>::get(config.key_range);
    const auto& keys = data.keys;

    std::cout << "Testing: " << Cache::name() << (UseYield ? " (with yield)" : "") << "..." << std::endl;
    std::atomic<bool> start_signal{false};

    { // Warming up Cache
        typename Cache::value_type val{42};
        for (int i = 0; i <= config.key_range; ++i) {
            cache.put(i, val);
        }
    }

    for (int i = 0; i < config.readers; ++i) {
        threads.emplace_back([&cache, &config, &total_misses, &keys, &start_signal, i]() {
            unsigned long long local_misses = 0;
            std::size_t offset = (i * 100) & (config.key_amount - 1); // Distribute readers across different areas

            while(!start_signal.load(std::memory_order_acquire));

            for (long long j = 0; j < config.iterations; ++j) {
                auto res = cache.get(keys[(offset + j) & (config.key_amount - 1)]);
                if (!res) [[unlikely]] {
                    local_misses++;
                }
                if constexpr (UseYield) std::this_thread::yield();
            }
            total_misses.fetch_add(local_misses, std::memory_order_relaxed);
        });
    }

    for (int i = 0; i < config.writers; ++i) {
        threads.emplace_back([&cache, &config, &keys, &start_signal, i]() {
            typename Cache::value_type val{42};

            while(!start_signal.load(std::memory_order_acquire));

            for (long long j = 0; j < config.iterations; ++j) {
                std::size_t offset = ((config.readers + i) * 100) & (config.key_amount - 1); // Distribute writers across different areas
                cache.put((keys[(offset + j) & (config.key_amount - 1)]), val);
                if constexpr (UseYield) std::this_thread::yield();
            }
        });
    }

//    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    start_signal.store(true, std::memory_order_release);
    auto start = std::chrono::high_resolution_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    double total_ops = (config.readers + config.writers) * config.iterations;
    double total_reads = (double)config.readers * config.iterations;
    double throughput = total_ops / diff.count();
    double avg_latency_ns = (diff.count() / total_ops) * 1e9;
    double miss_rate = (total_misses.load() / total_reads) * 100.0;

    std::cout << "Time: "           << diff.count() << " s \n"
              << "Ops/sec: "        << format_large_num(throughput) << "\n"
              << "Avg Latency: "    << avg_latency_ns << " ns\n"
              << "Misses: "         << format_large_num(total_misses.load())
              << " (" << std::fixed << std::setprecision(2) << miss_rate << "%)\n\n";
}

template<bool UseYield = false, typename... Caches>
void execute_scenario(const TestConfig& config) {

std::string scenario = "SCENARIO: Readers(" + std::to_string(config.readers) +
                       ") Writers(" + std::to_string(config.writers) +
                       ") Iterations: " + std::to_string(config.iterations / 1'000'000) + " M\n";
std::string mode = UseYield ? "| YIELD MODE |" : "| NORMAL MODE |";

std::cout << "========================================================\n"
          << std::setw(56) << std::left << scenario << "\n"
          << std::setw(19) << "" << mode << "\n"
          << "--------------------------------------------------------\n"
          << std::left << std::setw(16) << "  CacheSize:"    << std::right << std::setw(10) << config.cache_size
          << "   "     << std::left << std::setw(16) << "KeyRange:"      << std::right << std::setw(10) << config.key_range << "\n"
          << std::left << std::setw(16) << "  Payload Size:" << std::right << std::setw(10) << config.payload_size
          << "   "     << std::left << std::setw(16) << "Shards amount:" << std::right << std::setw(10) << config.shards_amount << "\n"
          << "========================================================\n" << std::endl;

    (run_benchmark<Caches, UseYield>(config), ...);

    std::cout << "Done: " << (config.readers + config.writers) << " threads finished.\n" << std::endl;
}

int main()
{
    const long long iters = 1e6;
    constexpr int cache_sz = 64 * 1024;
    constexpr int k_range = cache_sz * 120 / 100;
    const int payload_size = 64 * 1024;
    const int shards_amount = 32;

    using DataType = Payload<payload_size>;

    TestConfig read_heavy  = {28, 4, cache_sz, k_range, key_amount, iters, payload_size, shards_amount};
    TestConfig write_heavy = {4, 12, cache_sz, k_range, key_amount, iters, payload_size, shards_amount};
    TestConfig balanced    = {4, 2, cache_sz, k_range, key_amount, iters, payload_size, shards_amount};


    using Slow = StrictLRU<int, DataType, cache_sz>;
    using Spin = SpinlockedLRU<int, DataType, cache_sz>;
    using Def  = DeferredLRU<int, DataType, cache_sz>;
    using DefFM = DeferredFlatLRU<int, DataType, cache_sz>;
    using Lv1_bdFM = Lv1_bdFlatLRU<int, DataType, cache_sz>;
    using Lv2_bdFM = Lv2_bdFlatLRU<int, DataType, cache_sz>;
    using Lv3_bdFM = Lv3_bdFlatLRU<int, DataType, cache_sz>;
    using Lv4_bdFM = Lv4_bdFlatLRU<int, DataType, cache_sz>;
    using Lv5_bdFM = Lv5_bdFlatLRU<int, DataType, cache_sz>;
//    using Lv6_bdFM = Lv6_bdFlatLRU<int, DataType, cache_sz>;

    using S_Slow = ShardedCache<StrictLRU, int, DataType, cache_sz, shards_amount>;
    using S_Spin = ShardedCache<SpinlockedLRU, int, DataType, cache_sz, shards_amount>;
    using S_Def  = ShardedCache<DeferredLRU, int, DataType, cache_sz, shards_amount>;
    using S_DefFM = ShardedCache<DeferredFlatLRU, int, DataType, cache_sz, shards_amount>;
    using S_Lv1_bdFM = ShardedCache<Lv1_bdFlatLRU, int, DataType, cache_sz, shards_amount>;
    using S_Lv2_bdFM = ShardedCache<Lv2_bdFlatLRU, int, DataType, cache_sz, shards_amount>;
    using S_Lv3_bdFM = ShardedCache<Lv3_bdFlatLRU, int, DataType, cache_sz, shards_amount>;
    using S_Lv4_bdFM = ShardedCache<Lv4_bdFlatLRU, int, DataType, cache_sz, shards_amount>;
    using S2_Lv4_bdFM = Lv2_ShardedCache<Lv4_bdFlatLRU, int, DataType, cache_sz, shards_amount>;
    using S3_Lv5_bdFM = Lv3_ShardedCache<Lv5_bdFlatLRU, int, DataType, cache_sz, shards_amount>;
//    using S4_Lv6_bdFM = Lv4_ShardedCache<Lv6_bdFlatLRU, int, DataType, cache_sz, shards_amount>;

//    execute_scenario<false, Slow, Spin, Def, DefFM, Lv1_bdFM, Lv2_bdFM, Lv3_bdFM, Lv4_bdFM, Lv5_bdFM, S_Slow, S_Spin, S_Def, S_DefFM, S_Lv1_bdFM, S_Lv2_bdFM, S_Lv3_bdFM, S2_Lv4_bdFM, S3_Lv5_bdFM>(balanced);
//    execute_scenario<false, Slow, Spin, Def, DefFM, Lv1_bdFM, Lv2_bdFM, Lv3_bdFM, Lv4_bdFM, Lv5_bdFM, S_Slow, S_Spin, S_Def, S_DefFM, S_Lv1_bdFM, S_Lv2_bdFM, S_Lv3_bdFM, S2_Lv4_bdFM, S3_Lv5_bdFM>(write_heavy);
//    execute_scenario<false, Slow, Spin, Def, DefFM, Lv1_bdFM, Lv2_bdFM, Lv3_bdFM, Lv4_bdFM, Lv5_bdFM, S_Slow, S_Spin, S_Def, S_DefFM, S_Lv1_bdFM, S_Lv2_bdFM, S_Lv3_bdFM, S2_Lv4_bdFM, S3_Lv5_bdFM>(read_heavy);

    execute_scenario<false, /*S_Lv3_bdFM, */S2_Lv4_bdFM, S3_Lv5_bdFM>(read_heavy);
//    execute_scenario<false, /*S_Lv3_bdFM, */S2_Lv4_bdFM, S3_Lv5_bdFM, S4_Lv6_bdFM>(read_heavy);
/*
    execute_scenario<true, Slow, Spin, Def, DefFM, Lv1_bdFM, S_Slow, S_Spin, S_Def, S_DefFM, S_Lv1_bdFM>(balanced);
    execute_scenario<true, Slow, Spin, Def, DefFM, Lv1_bdFM, S_Slow, S_Spin, S_Def, S_DefFM, S_Lv1_bdFM>(write_heavy);
    execute_scenario<true, Slow, Spin, Def, DefFM, Lv1_bdFM, S_Slow, S_Spin, S_Def, S_DefFM, S_Lv1_bdFM>(read_heavy);
*/
    return 0;
}
