#include <vector>
#include <algorithm>
#include <numeric>
#include <ranges>
#include <functional>
#include <iostream>
#include <cassert>
#include <chrono>

struct Data {
    uint32_t priority;
    uint32_t value;
};

using DataRef = std::reference_wrapper<const Data>;

std::vector<std::vector<DataRef>> makePlaning(  uint32_t const totalMaxValue,
                                                uint32_t const totalMaxCount,
                                                std::vector<Data> const& input)
{
    if (input.empty()) return {};

    std::vector<DataRef> sortedRefs;
    sortedRefs.reserve(input.size());
    for (const auto& item : input) sortedRefs.emplace_back(item);

    std::ranges::sort(sortedRefs, [](const Data& a, const Data& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.value > b.value;
    });// O(N log N) or O(N^2)

    std::vector<std::vector<DataRef>> res;
    std::vector<bool> used(sortedRefs.size(), false);
    uint32_t remaining = sortedRefs.size();

     while (remaining > 0) { // O (N)
        auto& currentBatch = res.emplace_back();
        currentBatch.reserve(totalMaxCount);
        uint64_t currentSumValue = 0;

        for (std::size_t i = 0; i < sortedRefs.size(); ++i) {
            if (used[i]) continue;

            const Data& item = sortedRefs[i].get();

            if (currentBatch.size() < totalMaxCount && (currentSumValue + item.value) <= totalMaxValue) {
                currentBatch.push_back(sortedRefs[i]);
                currentSumValue += item.value;
                used[i] = true;
                remaining--;

                if (currentBatch.size() == totalMaxCount || currentSumValue == totalMaxValue)
                    break;
            }
        }

        if (currentBatch.empty()) break;
    }

    return res;
}

void run_tests() {
    const uint32_t totalMaxValue  = 1000;
    const uint32_t totalMaxCount = 3;

    {
        std::vector<Data> input = {{100, 500}, {100, 500}, {50, 300}, {50, 300}, {50, 300}};
        auto plan = makePlaning(totalMaxValue, 3, input);
        assert(plan.size() <= 3);
        std::cout << "Test 1 (Basic): PASSED\n";
    }
/*
    {
        std::vector<Data> input = {{100, 950}, {40, 300}, {40, 300}, {40, 300}};
        auto plan = makePlaning(totalMaxValue, 3, input);
        assert(plan.size() <= 2);
        assert(plan[0].size() == 3);
        std::cout << "Test 2 (Inversion of order): PASSED\n";
    }

    {
        std::vector<Data> input = {{100, 1500}, {100, 200}};
        auto plan = makePlaning(1000, 5, input);
        assert(plan.size() == 1); 
        assert(plan[0][0].get().value == 200);
        std::cout << "Test 3 (Over-totalMaxValue): PASSED\n";
    }
*/
    {
        std::vector<Data> input = {{1, 100}, {10, 100}, {5, 100}, {10, 100}};
        auto plan = makePlaning(totalMaxValue, 2, input);
        
        for(const auto& item : plan[0]) {
            assert(item.get().priority == 10);
        }
        std::cout << "Test 4 (Priority Strictness): PASSED\n";
    }

    {
        std::vector<Data> input;
        for(int i = 0; i < 10000; ++i) input.push_back({uint32_t(i % 100), 10});

        auto start = std::chrono::steady_clock::now();
        auto plan = makePlaning(100, 10, input);
        auto end = std::chrono::steady_clock::now();
        
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Test 5 (Stress 10k): PASSED in " << diff.count() << "ms\n";
    }

    {
        auto plan = makePlaning(totalMaxValue, 10, {});
        assert(plan.empty());
        std::cout << "Test 6 (Empty): PASSED\n";
    }

    {
        std::vector<Data> input = {
            {100, 950},
            {90, 100},
            {80, 100}
        };
        auto plan = makePlaning(totalMaxValue, totalMaxCount, input);
        assert(plan.size() == 2);
        assert(plan[0].size() == 1);
        assert(plan[0][0].get().priority == 100);
        std::cout << "Test 7 (Fat High-Priority): PASSED\n";
    }

    {
        std::vector<Data> input = {
            {100, 950},
            {90, 100},
            {80, 100}
        };
        auto plan = makePlaning(totalMaxValue, totalMaxCount, input);
        assert(plan.size() == 2);
        assert(plan[0].size() == 1);
        assert(plan[0][0].get().priority == 100);
        std::cout << "Test 8 (Fat High-Priority): PASSED\n";
    }

    {
        std::vector<Data> input = {
            {100, 800},
            {90, 800},
            {10, 100}

        };
        auto plan = makePlaning(totalMaxValue, totalMaxCount, input);
        assert(plan[0].size() == 2); // {100, 800} и {10, 100}
        assert(plan[0][1].get().priority == 10); 
        std::cout << "Test 9 (Gap Filling): PASSED\n";
    }
/*
    {
        std::vector<Data> input = { {100, 2000}, {50, 100} };
        auto plan = makePlaning(totalMaxValue, totalMaxCount, input);
        assert(plan.size() == 1);
        assert(plan[0][0].get().value == 100);
        std::cout << "Test 10 (Over-totalMaxValue Item): PASSED\n";
    }
*/
    {
        std::vector<Data> input(10, {10, 10});
        auto plan = makePlaning(totalMaxValue, 3, input);
        
        assert(plan.size() == 4);
        assert(plan[0].size() == 3);
        assert(plan[3].size() == 1);
        std::cout << "Test 11 (MaxCount Limit): PASSED\n";
    }
}

int main() {

    run_tests();

    return 0;
}
