#include <vector>
#include <algorithm>
#include <numeric>
#include <ranges>
#include <functional>
#include <iostream>
#include <cassert>

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

     while (remaining > 0) {
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

int main() {
    uint32_t const totalMaxValue = 1000;
    uint32_t const totalMaxCount = 3;

    std::vector<Data> input1 = { {100, 500}, {100, 500}, {50, 300}, {50, 300}, {50, 300} };

    auto output = makePlaning(totalMaxValue, totalMaxCount, input1);

    auto lastPriority = std::numeric_limits<uint32_t>::max();
    for (const auto& iteration : output) {
        assert(iteration.size() <= totalMaxCount);

        uint64_t sumValue = std::accumulate(iteration.begin(), iteration.end(), 0ull, 
            [](uint64_t sum, const Data& d) { return sum + d.value; });
        assert(sumValue <= totalMaxValue);

        auto currentPriority = std::accumulate(iteration.begin(), iteration.end(), 0ull,
            [](uint64_t sum, const Data& d) {return sum + d.priority;});
        assert(currentPriority <= lastPriority);

        lastPriority = currentPriority;
    }

    return 0;
}
