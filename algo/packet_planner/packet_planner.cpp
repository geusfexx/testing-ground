#include <vector>
#include <algorithm>
#include <numeric>
#include <ranges>
#include <functional>
#include <iostream>
#include <cassert>
#include <chrono>

struct Packet {
    uint32_t priority;
    uint32_t payload;
};

enum class MTUViolationPolicy {
    Drop,
    Fragment
};

using PacketRef = std::reference_wrapper<const Packet>;
using Frame = std::vector<PacketRef>;

std::vector<Frame> mapQosToFrameSequence( uint32_t const MTU,
                                uint32_t const maxPacketsPerFrame,
                                std::vector<Packet> const& txQueue,
                                MTUViolationPolicy policy = MTUViolationPolicy::Drop)
{
    if (txQueue.empty()) return {};

    Frame inputBuffer;
    inputBuffer.reserve(txQueue.size());
    for (const auto& pkt : txQueue) {
        if (pkt.payload > MTU) {
            if (policy == MTUViolationPolicy::Fragment) {
                //TODO
            }
            if (policy == MTUViolationPolicy::Drop) {
                continue;
            }
        }
        inputBuffer.emplace_back(pkt);
    }

    std::ranges::sort(inputBuffer, [](const Packet& a, const Packet& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.payload > b.payload;
    });// O(N log N) or O(N^2)

    std::vector<Frame> frameSequence;
    std::vector<bool> used(inputBuffer.size(), false);
    uint32_t remaining = inputBuffer.size();

     while (remaining > 0) { // O (N^2)
        auto& currentBatch = frameSequence.emplace_back();
        currentBatch.reserve(maxPacketsPerFrame);
        uint64_t currentSumValue = 0;

        for (std::size_t i = 0; i < inputBuffer.size(); ++i) {
            if (used[i]) continue;

            const Packet& item = inputBuffer[i].get();

            if (currentBatch.size() < maxPacketsPerFrame && (currentSumValue + item.payload) <= MTU) {
                currentBatch.push_back(inputBuffer[i]);
                currentSumValue += item.payload;
                used[i] = true;
                remaining--;

                if (currentBatch.size() == maxPacketsPerFrame || currentSumValue == MTU)
                    break;
            }
        }

        if (currentBatch.empty()) break;
    }

    return frameSequence;
}

void run_tests() {
    const uint32_t MTU  = 1000;
    const uint32_t maxPacketsPerFrame = 3;

    {
        std::vector<Packet> input = {{100, 500}, {100, 500}, {50, 300}, {50, 300}, {50, 300}};
        auto plan = mapQosToFrameSequence(MTU, 3, input);
        assert(plan.size() <= 3);
        std::cout << "Test 1 (Basic): PASSED\n";
    }
/*
    {
        std::vector<Packet> input = {{100, 950}, {40, 300}, {40, 300}, {40, 300}};
        auto plan = mapQosToFrameSequence(MTU, 3, input);
        assert(plan.size() <= 2);
        assert(plan[0].size() == 3);
        std::cout << "Test 2 (Inversion of order): PASSED\n";
    }
*/
    {
        std::vector<Packet> input = {{100, 1500}, {100, 200}};
        auto plan = mapQosToFrameSequence(1000, 5, input);
        assert(plan.size() == 1); 
        assert(plan[0][0].get().payload == 200);
        std::cout << "Test 3 (Over-MTU): PASSED\n";
    }

    {
        std::vector<Packet> input = {{1, 100}, {10, 100}, {5, 100}, {10, 100}};
        auto plan = mapQosToFrameSequence(MTU, 2, input);
        
        for(const auto& item : plan[0]) {
            assert(item.get().priority == 10);
        }
        std::cout << "Test 4 (Priority Strictness): PASSED\n";
    }

    {
        std::vector<Packet> input;
        for(int i = 0; i < 10000; ++i) input.push_back({uint32_t(i % 100), 10});

        auto start = std::chrono::steady_clock::now();
        auto plan = mapQosToFrameSequence(100, 10, input);
        auto end = std::chrono::steady_clock::now();
        
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Test 5 (Stress 10k): PASSED in " << diff.count() << "ms\n";
    }

    {
        auto plan = mapQosToFrameSequence(MTU, 10, {});
        assert(plan.empty());
        std::cout << "Test 6 (Empty): PASSED\n";
    }

    {
        std::vector<Packet> input = {
            {100, 950},
            {90, 100},
            {80, 100}
        };
        auto plan = mapQosToFrameSequence(MTU, maxPacketsPerFrame, input);
        assert(plan.size() == 2);
        assert(plan[0].size() == 1);
        assert(plan[0][0].get().priority == 100);
        std::cout << "Test 7 (Fat High-Priority): PASSED\n";
    }

    {
        std::vector<Packet> input = {
            {100, 950},
            {90, 100},
            {80, 100}
        };
        auto plan = mapQosToFrameSequence(MTU, maxPacketsPerFrame, input);
        assert(plan.size() == 2);
        assert(plan[0].size() == 1);
        assert(plan[0][0].get().priority == 100);
        std::cout << "Test 8 (Fat High-Priority): PASSED\n";
    }

    {
        std::vector<Packet> input = {
            {100, 800},
            {90, 800},
            {10, 100}

        };
        auto plan = mapQosToFrameSequence(MTU, maxPacketsPerFrame, input);
        assert(plan[0].size() == 2); // {100, 800} и {10, 100}
        assert(plan[0][1].get().priority == 10); 
        std::cout << "Test 9 (Gap Filling): PASSED\n";
    }

    {
        std::vector<Packet> input = { {100, 2000}, {50, 100} };
        auto plan = mapQosToFrameSequence(MTU, maxPacketsPerFrame, input);
        assert(plan.size() == 1);
        assert(plan[0][0].get().payload == 100);
        std::cout << "Test 10 (Over-MTU Item): PASSED\n";
    }

    {
        std::vector<Packet> input(10, {10, 10});
        auto plan = mapQosToFrameSequence(MTU, 3, input);
        
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
