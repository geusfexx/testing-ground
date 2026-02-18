#include <vector>
#include <algorithm>
#include <numeric>
#include <ranges>
#include <functional>
#include <iostream>
#include <cassert>
#include <chrono>
#include <concepts>
#include <string_view>
#include <execution>
#include <span>

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
using SchedulingPolicy = std::function<bool(const Packet&, const Packet&)>;

namespace Policies {

    struct StrictPriority {
        bool operator()(const Packet& a, const Packet& b) const {
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.payload > b.payload;
        }
    };

    struct WeightedEfficiency {
        bool operator()(const Packet& a, const Packet& b) const {
            return static_cast<double>(a.priority) / a.payload >
                   static_cast<double>(b.priority) / b.payload;
        }
    };
}
/*
template<typename F>
concept SchedulerFunc = requires(F f, uint32_t m, uint32_t c, std::vector<Packet> q) {
    { f(m, c, q, MTUViolationPolicy::Drop, Policies::StrictPriority) } -> std::same_as<std::vector<Frame>>;
};
*/

struct FlatFrameSequence {
    std::vector<PacketRef> allPackets;
    std::vector<std::size_t> frameOffsets; // Indexes

    std::span<const PacketRef> getFrame(std::size_t index) const {
        std::size_t start = frameOffsets[index];
        std::size_t end = (index + 1 < frameOffsets.size()) ? frameOffsets[index + 1] : allPackets.size();
        return std::span<const PacketRef>(allPackets.data() + start, end - start);
    }

    std::size_t frameCount() const { return frameOffsets.size(); }
};

// First Fit
template<typename SchedPolicy = Policies::StrictPriority>
std::vector<Frame> mapQosToFrameSequence( uint32_t const MTU,
                                uint32_t const maxPacketsPerFrame,
                                std::vector<Packet> const& txQueue,
                                MTUViolationPolicy MTUpolicy = MTUViolationPolicy::Drop,
                                SchedPolicy schedPolicy = {})
{
    if (txQueue.empty()) return {};

    Frame inputBuffer;
    inputBuffer.reserve(txQueue.size());
    for (const auto& pkt : txQueue) {
        if (pkt.payload > MTU) {
            if (MTUpolicy == MTUViolationPolicy::Fragment) {
                //TODO
            }
            if (MTUpolicy == MTUViolationPolicy::Drop) {
                continue;
            }
        }
        inputBuffer.emplace_back(pkt);
    }

    std::ranges::sort(inputBuffer, schedPolicy); // O(N log N) or O(N^2)

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

// Next Fit
template<typename SchedPolicy = Policies::StrictPriority>
FlatFrameSequence mapQosToFrameSequenceFast(
    uint32_t const MTU,
    uint32_t const maxPacketsPerFrame,
    std::vector<Packet> const& txQueue,
    MTUViolationPolicy MTUpolicy = MTUViolationPolicy::Drop,
    SchedPolicy schedPolicy = {})
{
    if (txQueue.empty()) return {};

    // O(N)
    Frame inputBuffer;
    inputBuffer.reserve(txQueue.size());
    for (const auto& pkt : txQueue) {
        if (pkt.payload <= MTU) {
            inputBuffer.emplace_back(pkt);
        } else if (MTUpolicy == MTUViolationPolicy::Fragment) {
            //TODO
        }
    }

//    std::ranges::sort(inputBuffer, schedPolicy); // O(N log N) or O(N^2)
    std::sort(std::execution::unseq, inputBuffer.begin(), inputBuffer.end(), schedPolicy); // O(N log N)

    FlatFrameSequence frameSequence;
    frameSequence.allPackets.reserve(inputBuffer.size());
    frameSequence.frameOffsets.reserve(inputBuffer.size() / 2);
    if (inputBuffer.empty()) return {};

    uint64_t currentFramePayload = 0;
    uint32_t currentFrameCount = 0;

    // O(N)
    for (const auto& pktRef : inputBuffer) {
        const Packet& pkt = pktRef.get();

        if (currentFrameCount == 0 ||
            currentFrameCount >= maxPacketsPerFrame ||
            (currentFramePayload + pkt.payload) > MTU)
        {
            frameSequence.frameOffsets.push_back(frameSequence.allPackets.size());
            currentFramePayload = 0;
            currentFrameCount = 0;
        }

        frameSequence.allPackets.push_back(pktRef);
        currentFramePayload += pkt.payload;
        currentFrameCount++;
    }

    return frameSequence;
}

void printHeader(std::string_view schedulerName) {
    constexpr uint32_t width = 42;
    const uint32_t padding = (width - 2 - schedulerName.length()) / 2;

    std::cout << "\n" << std::string(width, '=') << "\n";
    std::cout << "=" << std::string(padding, ' ') << schedulerName
              << std::string(width - 2 - padding - schedulerName.length(), ' ') << "=\n";
    std::cout << std::string(width, '=') << "\n\n";
}

template<typename T>
size_t get_size(const T& plan) {
    if constexpr (requires { plan.size(); }) return plan.size();
    else return plan.frameCount();
}

template<typename T>
auto get_frame(const T& plan, size_t idx) {
    if constexpr (requires { plan[idx]; }) return plan[idx];
    else return plan.getFrame(idx);
}

auto FirstFitCaller = [](auto mtu, auto count, const auto& queue, auto mtuPolicy, auto schedPolicy) {
    return mapQosToFrameSequence(mtu, count, queue, mtuPolicy, schedPolicy);
};

auto NextFitCaller = [](auto mtu, auto count, const auto& queue, auto mtuPolicy, auto schedPolicy) {
    return mapQosToFrameSequenceFast(mtu, count, queue, mtuPolicy, schedPolicy);
};

template<typename TCaller>
void run_tests(TCaller scheduler, std::string_view schedulerName) {
    printHeader(schedulerName);

    const uint32_t MTU  = 1000; //NOTE Thrust me, I know :p
    const uint32_t maxPacketsPerFrame = 3;

    // Test 1: Basic
    {
        std::vector<Packet> input = {{100, 500}, {100, 500}, {50, 300}, {50, 300}, {50, 300}};
        auto plan = scheduler(MTU, maxPacketsPerFrame, input,
                              MTUViolationPolicy::Drop, Policies::StrictPriority{});
        assert(get_size(plan) >= 2);
        std::cout << "Test 1 (Basic): PASSED\n";
    }

    // Test 2: Inversion of order (QoS Weighted)
    {
        std::vector<Packet> input = {{100, 950}, {40, 300}, {40, 300}, {40, 300}};
        auto plan = scheduler(MTU, maxPacketsPerFrame, input,
                              MTUViolationPolicy::Drop, Policies::WeightedEfficiency{});

        auto frame0 = get_frame(plan, 0);
        assert(frame0.size() == 3);
        std::cout << "Test 2 (Inversion of order): PASSED\n";
    }

    // Test 3: Over-MTU Management
    {
        std::vector<Packet> input = {{100, 1500}, {100, 200}};
        auto plan = scheduler(MTU, maxPacketsPerFrame, input,
                              MTUViolationPolicy::Drop, Policies::StrictPriority{});
        assert(get_size(plan) == 1);
        assert(get_frame(plan, 0)[0].get().payload == 200);
        std::cout << "Test 3 (Over-MTU): PASSED\n";
    }

    // Test 4: Priority Strictness
    {
        std::vector<Packet> input = {{1, 100}, {10, 100}, {5, 100}, {10, 100}};
        auto plan = scheduler(MTU, 2, input,
                              MTUViolationPolicy::Drop, Policies::StrictPriority{});
        
        auto frame0 = get_frame(plan, 0);
        for(const auto& item : frame0) {
            assert(item.get().priority == 10);
        }
        std::cout << "Test 4 (Priority Strictness): PASSED\n";
    }

    // Test 5: Real Stress Test (100k packets)
    {
        std::vector<Packet> input;
        const int numPackets = 1e5;
        input.reserve(numPackets);
        for(int i = 0; i < numPackets; ++i) input.push_back({uint32_t(i % 100), 10});

        auto start = std::chrono::steady_clock::now();
        auto plan = scheduler(100, 10, input,
                              MTUViolationPolicy::Drop, Policies::StrictPriority{});
        auto end = std::chrono::steady_clock::now();
        
        auto diff = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        std::cout << "Test 5 (Stress 100k): PASSED in " << diff.count() / 1000.0 << "ms\n";
    }

    // Test 6: Empty Input
    {
        std::vector<Packet> input;
        auto plan = scheduler(MTU, maxPacketsPerFrame, input,
                              MTUViolationPolicy::Drop, Policies::StrictPriority{});
        assert(get_size(plan) == 0);
        std::cout << "Test 6 (Empty): PASSED\n";
    }

    // Test 7: Fat High-Priority
    {
        std::vector<Packet> input = {{100, 950}, {90, 100}, {80, 100}};
        auto plan = scheduler(MTU, maxPacketsPerFrame, input,
                              MTUViolationPolicy::Drop, Policies::StrictPriority{});
        assert(get_size(plan) == 2);
        assert(get_frame(plan, 0).size() == 1);
        assert(get_frame(plan, 0)[0].get().priority == 100);
        std::cout << "Test 7 (Fat High-Priority): PASSED\n";
    }

    // Test 8: Gap Filling (Only for First Fit)
    {
        std::vector<Packet> input = {{100, 800}, {90, 800}, {10, 100}};
        auto plan = scheduler(MTU, maxPacketsPerFrame, input,
                              MTUViolationPolicy::Drop, Policies::StrictPriority{});

        if (schedulerName.find("First Fit") != std::string_view::npos) {
            assert(get_frame(plan, 0).size() == 2);
            std::cout << "Test 8 (Gap Filling): PASSED\n";
        } else {
            assert(get_frame(plan, 0).size() == 1);
            std::cout << "Test 8 (Next Fit Behavior): PASSED\n";
        }
    }

    // Test 9: Burst Limit (MaxCount)
    {
        std::vector<Packet> input(10, {10, 10});
        auto plan = scheduler(MTU, maxPacketsPerFrame, input,
                              MTUViolationPolicy::Drop, Policies::StrictPriority{});
        assert(get_size(plan) == 4);
        std::cout << "Test 9 (MaxCount Limit): PASSED\n";
    }
    std::cout << "\n";
}

int main() {

    run_tests(FirstFitCaller, "First Fit (O(N^2))");
    run_tests(NextFitCaller, "Next Fit Fast (O(N log N))");

    return 0;
}
