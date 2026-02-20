#include <optional>
#include <unordered_map>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <cassert>
#include <bit>
#include <concepts>
#include <sys/mman.h>
#include <cstddef>

/*              Up to 32-64 (?) cores (With NUMA penalty)
*   TODO:   Summary       Final compatible to Lv5
*   TODO:                       Full compatibility: The get / put are identical to Lv5.     //NOTE
*   TODO:                       Housekeeper (basic)                                         //TODO
*   TODO:                       Coalescing Filter                                           //TODO
*   TODO:                       Lock-Free Shard Ownership                                   //TODO
*   TODO:                       Telemetry                                                   //TODO
*   TODO:                       Arena Finalization                                          //TODO
*   TODO:                       Flatmap Optimization                                        //TODO
*/

struct DirtyArena { //FIXME only to test the hypothesis
    uint8_t* ptr;
    std::atomic<size_t> offset{0};
    size_t capacity;
    struct Node { Node* next; };
    std::atomic<Node*> free_list{nullptr};

    static inline constexpr size_t PageSize = 2 * sizes::MiB;
//    static_assert(sizeof(T) >= sizeof(Node));

    DirtyArena() {
        capacity = 1024 * PageSize; // 2 GiB
        ptr = (uint8_t*)mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr == MAP_FAILED) {
            ptr = nullptr;
            capacity = 0;
        }
    }
    ~DirtyArena() { if (ptr) munmap(ptr, capacity); }
};

//CRITICAL  Arena must be the only one for ANY template instances
//          to avoid segfault
inline DirtyArena& get_global_arena() {
    static DirtyArena arena;
    return arena;
}

template <typename T>
struct HugePagesAllocator {
    using value_type = T;
    static inline constexpr size_t PageSize = 2 * sizes::MiB;

    template <typename U> struct rebind { using other = HugePagesAllocator<U>; };
    HugePagesAllocator() noexcept = default;
    template <typename U> HugePagesAllocator(const HugePagesAllocator<U>&) noexcept {}

    [[nodiscard]] static DirtyArena& get_arena() noexcept {
        return get_global_arena();
    }

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n == 0) [[unlikely]] return nullptr;
        auto& arena = get_arena();

        if (n == 1) {
            auto* head = arena.free_list.load(std::memory_order_acquire);
            while (head && !arena.free_list.compare_exchange_weak(head, head->next,
                                                                std::memory_order_acq_rel)) {
                // CAS Loop
            }
            if (head) return reinterpret_cast<T*>(head);
        }

        size_t bytes = n * sizeof(T);
        size_t current_offset = arena.offset.fetch_add(bytes, std::memory_order_relaxed);

        if (!arena.ptr || current_offset + bytes > arena.capacity) [[unlikely]] {
            // If Huge Pages are out of space or not supported: malloc
            void* fallback = std::malloc(bytes);
            if (!fallback) throw std::bad_alloc();
            return static_cast<T*>(fallback);
        }

        return reinterpret_cast<T*>(arena.ptr + current_offset);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (!p) [[unlikely]] return;
        auto& arena = get_arena();

        if (p >= (T*)arena.ptr && p < (T*)(arena.ptr + arena.capacity)) {
            if (n == 1) {
                auto* node = reinterpret_cast<typename DirtyArena::Node*>(p);
                auto* old_head = arena.free_list.load(std::memory_order_relaxed);
                do {
                    node->next = old_head;
                } while (!arena.free_list.compare_exchange_weak(old_head, node,
                                                               std::memory_order_release));
            }
            // Multi-block allocations (n ​​> 1) are not reused in this arena.
        } else {
            std::free(p);
        }
    }

    friend bool operator==(const HugePagesAllocator&, const HugePagesAllocator&) = default;
};

template <typename T, typename Alloc = HugePagesAllocator<T>>
class FlatStorage {
    T* _data;
    std::size_t _n;
    [[no_unique_address]] Alloc _alloc;

public:
    explicit FlatStorage(std::size_t n) : _n(n) {
        _data = _alloc.allocate(_n);

        for (std::size_t i = 0; i < _n; ++i) {
            std::construct_at(&_data[i]);
        }
    }

    ~FlatStorage() {
        std::destroy_n(_data, _n);
        _alloc.deallocate(_data, _n);
    }

    T& operator[](std::size_t i) { return _data[i]; }
    const T& operator[](std::size_t i) const noexcept { return _data[i]; }
    std::size_t size() const noexcept { return _n; }

    void prefetch(std::size_t i) const noexcept {
        sizes::prefetch(&_data[i]);
    }
};

template <Hashable KeyType, typename ValueType, std::size_t Capacity = 1024, typename Alloc = HugePagesAllocator<char>>
requires PowerOfTwoValue<Capacity>
class Lv4_LinkedFlatMap : private NonCopyableNonMoveable { // Open Addressing table with Linear Probing
public:
    using index_type = std::conditional_t<(Capacity <= 65535), uint16_t, uint32_t>;
    static constexpr index_type NullIdx = std::numeric_limits<index_type>::max();
    static constexpr std::size_t CacheLine = sizes::CacheLine;

    enum class slot_state : uint8_t { Empty = 0, Occupied = 1, Deleted = 2 };

    static constexpr const char* name() noexcept { return "Lv4_LinkedFlatMap"; }
    using value_type = ValueType;
    using key_type = KeyType;
    using value_ptr = std::shared_ptr<ValueType>;

private:

    struct alignas(CacheLine / 2) MetaEntry {
        // Group 1: Metadata (Hot)                          8 bytes
        std::atomic<uint32_t>  gen{0};
        std::atomic<slot_state> state{slot_state::Empty};

        // Group 2: Search (Hot)                            8 bytes
        KeyType key;

        // Group 3: LRU Links (Warm)                        8 bytes
        index_type next = NullIdx;
        index_type prev = NullIdx;
    };

    struct DataEntry {
        // exGroup 4: Data (Cold/Warm)
        value_ptr value;                                    // sizeof(value_type)
    };

public:
    const MetaEntry& get_meta(index_type idx) const noexcept {
        return _meta_table[idx];
    }

    MetaEntry& get_meta_mutable(index_type idx) noexcept {
        return _meta_table[idx];
    }

    const DataEntry& get_data(index_type idx) const noexcept {
        return _data_table[idx];
    }

    DataEntry& get_data_mutable(index_type idx) noexcept {
        return _data_table[idx];
    }

private:
    static_assert(std::has_single_bit(Capacity), "Capacity must be power of 2");
    static constexpr std::size_t TableSize = Capacity * 2; // Load factor 0.5
    static_assert(TableSize == Capacity * 2, "Load factor must be 0.5");
    static constexpr std::size_t Mask = TableSize - 1;
    using MetaAlloc = typename std::allocator_traits<Alloc>::template rebind_alloc<MetaEntry>;
    using DataAlloc = typename std::allocator_traits<Alloc>::template rebind_alloc<DataEntry>;
private:

    struct LookupResult {
        value_ptr  ptr;
        index_type idx;
        uint32_t   gen;
    };

    std::size_t calculate_hash_idx(const KeyType& key) const noexcept {
        return std::hash<KeyType>{}(key) & Mask;
    }

    std::size_t next_slot(std::size_t current_idx) const noexcept {
        return (current_idx + 1) & Mask;
    }

    void detach(const index_type& idx) noexcept {
        auto& meta = _meta_table[idx];
        const index_type n = meta.next;
        const index_type p = meta.prev;

        if (n != NullIdx) [[likely]] { _meta_table[n].prev = p; }
        else _tail = p;

        if (p != NullIdx) [[likely]] { _meta_table[p].next = n; }
        else _head = n;

        meta.next = NullIdx;
        meta.prev = NullIdx;
    }

    void push_front(index_type idx) noexcept {
        auto& meta = _meta_table[idx];
        const index_type old_head = _head;

        meta.next = _head;
        meta.prev = NullIdx;

        if (old_head != NullIdx) [[likely]] { _meta_table[old_head].prev = idx; }
        _head = idx;

        if (_tail == NullIdx) [[unlikely]] { _tail = idx; }
    }

public:

    Lv4_LinkedFlatMap() noexcept = default;

    value_ptr update_slot(index_type idx, value_ptr&& new_val) noexcept {
        auto& meta = _meta_table[idx];
        auto& data = _data_table[idx];

        uint32_t current_gen = meta.gen.load(std::memory_order_relaxed);
        meta.gen.store(current_gen + 1, std::memory_order_release); // lock by odd gen

        value_ptr old_ptr = std::move(data.value);
        data.value = std::move(new_val);

        meta.state.store(slot_state::Occupied, std::memory_order_release);
        meta.gen.store(current_gen + 2, std::memory_order_release);
        meta.gen.notify_all();

        return old_ptr;
    }

    bool is_occupied(index_type idx) const noexcept {
        return _meta_table[idx].state == slot_state::Occupied;
    }

    bool is_valid_gen(index_type idx, uint32_t gen) const noexcept {
        return _meta_table[idx].state == slot_state::Occupied && _meta_table[idx].gen == gen;
    }

    std::size_t size() const noexcept { return _size; }
    index_type get_tail() const noexcept { return _tail; }
    index_type get_head() const noexcept { return _head; }

    // Uses by writer (under lock)
    // Fast lookup: returns index and gen without copying shared_ptr
    LookupResult lookup(const KeyType& key) const noexcept {
        std::size_t idx = calculate_hash_idx(key);
        index_type first_del = NullIdx;

        _meta_table.prefetch(idx);

        while (true) {
            const auto& meta = _meta_table[idx];
            const auto state = meta.state.load(std::memory_order_relaxed);

            if (state == slot_state::Empty) {
                index_type target = (first_del != NullIdx) ? first_del : static_cast<index_type>(idx);
                return {nullptr, target, 0};
            }

            if (state == slot_state::Deleted) {
                if (first_del == NullIdx) first_del = static_cast<index_type>(idx);
            }

            if (state == slot_state::Occupied) {
                if (meta.key == key) {
                    return { _data_table[idx].value, static_cast<index_type>(idx), meta.gen.load(std::memory_order_relaxed) };
                }
            }

            idx = next_slot(idx);
            if constexpr (TableSize > 16) {
                _meta_table.prefetch((idx + 2) & Mask);
            }
        }

        // It can loop for eternity only if Load Factor > 1.0 (all slots are Occupied or Deleted)
        // But since it has Capacity * 2 and there is no deletion of Empty slots, this is impossible.
        assert(false && "LinkedFlatMap table size overflow or corrupted logic");
        __builtin_unreachable(); // I'm sure
    }

    // Uses by reader (lockless)
    LookupResult get_lockless(const KeyType& key) const noexcept {
        std::size_t idx = calculate_hash_idx(key);

        for (std::size_t i = 0; i < TableSize; ++i) {
            const auto& meta = _meta_table[idx];

            // 1. Acquire gen
            uint32_t gen1 = meta.gen.load(std::memory_order_acquire);
            if (gen1 & 1) [[unlikely]] {
                meta.gen.wait(gen1, std::memory_order_relaxed);
                gen1 = meta.gen.load(std::memory_order_acquire);
                if (gen1 & 1) return {nullptr, NullIdx, 0};
            } // value isn't actual

            const auto state = meta.state.load(std::memory_order_relaxed);
            if (state == slot_state::Empty) return {nullptr, NullIdx, 0};

            if (state == slot_state::Occupied) {
                std::atomic_ref<const KeyType> key_ref(meta.key);
                if (key_ref.load(std::memory_order_relaxed) == key) {
                    auto val_ref = _data_table[idx].value; //SAFETY Thrust me, I know what i'm doing

                    if (meta.gen.load(std::memory_order_acquire) == gen1) [[likely]] {
                        return {std::move(val_ref), static_cast<index_type>(idx), gen1};
                    }
                    return {nullptr, NullIdx, 0};
                }
            }
            idx = next_slot(idx);
        }
        return {nullptr, NullIdx, 0};
    }

    void emplace_at(index_type idx, const key_type& key, value_ptr&& new_ptr) noexcept {
        auto& meta = _meta_table[idx];
        auto& data = _data_table[idx];

        meta.gen.fetch_add(1, std::memory_order_release);    // This is important to avoid dirty read
        std::atomic_ref<KeyType> key_ref(meta.key);
        key_ref.store(key, std::memory_order_relaxed);       // Data race avoidance

        data.value = std::move(new_ptr); // Memory had been allocated by assign_slot

        meta.state.store(slot_state::Occupied, std::memory_order_release);
        meta.gen.fetch_add(1, std::memory_order_release);
        meta.gen.notify_all();
        _size++;
    }

    index_type assign_slot(const key_type& key) noexcept {
        std::size_t idx = calculate_hash_idx(key);
        index_type first_deleted = NullIdx;

        while (true) {
            const auto state = _meta_table[idx].state.load(std::memory_order_relaxed);

            if (state == slot_state::Empty) {
                return (first_deleted != NullIdx) ? first_deleted : static_cast<index_type>(idx);
            }

            if (state == slot_state::Deleted) {
                if (first_deleted == NullIdx) {
                    first_deleted = static_cast<index_type>(idx);
                }
            }

            idx = next_slot(idx);
        }

        assert(false && "LinkedFlatMap table size overflow or corrupted logic");
        __builtin_unreachable(); // I'm sure
    }

    void move_to_front(index_type idx) noexcept {
        if (idx == _head || idx == NullIdx) return;

        const index_type n = _meta_table[idx].next;
        const index_type p = _meta_table[idx].prev;

        if (n != NullIdx) sizes::prefetch(&_meta_table[n], 1);
        if (p != NullIdx) sizes::prefetch(&_meta_table[p], 1);

        detach(idx);
        push_front(idx);
    }

    void erase_index(const index_type& idx) noexcept {
        if (idx == NullIdx || _meta_table[idx].state != slot_state::Occupied) return;

        detach(idx);

        _meta_table[idx].gen.fetch_add(1, std::memory_order_release);

        // The object is alive as long as the reader holds it
        _data_table[idx].value = nullptr;

        _meta_table[idx].state.store(slot_state::Deleted, std::memory_order_relaxed);
        _meta_table[idx].gen.fetch_add(1, std::memory_order_release);
        _meta_table[idx].gen.notify_all();

        _size--;
    }

private:
    FlatStorage<MetaEntry, MetaAlloc> _meta_table{TableSize};
    FlatStorage<DataEntry, DataAlloc> _data_table{TableSize};
    index_type _head = NullIdx;
    index_type _tail = NullIdx;
    std::size_t _size = 0;
};

template <typename Derived, std::size_t MaxThreads>
class EpochManager {
    static constexpr std::size_t CacheLine = sizes::CacheLine;

    struct ThreadState {
        alignas(64) std::atomic<uint64_t> active_epoch{0};
    };

public:
    struct [[nodiscard]] Guard {
        EpochManager*   owner;
        std::size_t     tid;
        ~Guard() { owner->leave_epoch(tid); }
    };

    uint64_t current_epoch() const noexcept {
        return _global_epoch.load(std::memory_order_relaxed);
    }

    Guard enter_epoch(std::size_t tid) noexcept {
        _thread_states[tid].active_epoch.store(_global_epoch.load(std::memory_order_relaxed), std::memory_order_release);
        return {this, tid};
    }

    void leave_epoch(std::size_t tid) noexcept {
        _thread_states[tid].active_epoch.store(0, std::memory_order_release);
    }

    uint64_t bump_epoch() noexcept {
        return _global_epoch.fetch_add(1, std::memory_order_acq_rel);
    }

    uint64_t get_min_active() const noexcept {
        uint64_t current = _global_epoch.load(std::memory_order_acquire);
        uint64_t min_e = current;
        for (const auto& ts : _thread_states) {
            uint64_t e = ts.active_epoch.load(std::memory_order_acquire);
            if (e != 0 && e < min_e) min_e = e;
        }
        return min_e;
    }

private:
    std::array<ThreadState, MaxThreads> _thread_states{};
    std::atomic<uint64_t>               _global_epoch{1};
};

template <Hashable KeyType, typename ValueType, std::size_t Capacity = 4 * 1024, std::size_t MaxThreads = 32>
requires PowerOfTwoValue<MaxThreads>
class Lv6_bdFlatLRU :   public EpochManager<Lv6_bdFlatLRU<KeyType, ValueType, Capacity, MaxThreads>, MaxThreads>,
                        private NonCopyableNonMoveable {
public:
    static constexpr const char* name() noexcept { return "Lv5_SPSCBuffer_DeferredFlatLRU"; }
    using value_type = ValueType;
    using key_type = KeyType;

private:
    using cacheMap = Lv4_LinkedFlatMap<KeyType, ValueType, Capacity>;
    static constexpr std::size_t CacheLine = sizes::CacheLine;

    struct alignas(CacheLine) UpdateOp {
        cacheMap::index_type    idx;
        uint32_t                gen;
    };

    using SPSCBuffer = SPSC_RingBufferUltraFast<UpdateOp, Capacity / (4 * MaxThreads)>;
    using BaseEpochManager = EpochManager<Lv6_bdFlatLRU<KeyType, ValueType, Capacity, MaxThreads>, MaxThreads>;

    static_assert(std::has_single_bit(MaxThreads), "MaxThreads must be a power of 2!");

private:
    struct alignas(CacheLine) PaddedSPSC : public SPSCBuffer {
        char padding[CacheLine - (sizeof(SPSCBuffer) & (CacheLine - 1))];
    };

    struct RetiredObject {
        std::shared_ptr<ValueType> ptr;
        uint64_t epoch;
    };

private:

    static std::size_t get_thread_id() {
        static std::atomic<std::size_t> counter{0};

        // rollcall
        thread_local std::size_t id = std::numeric_limits<std::size_t>::max();

        if (id == std::numeric_limits<std::size_t>::max()) [[unlikely]] {
            id = counter.fetch_add(1, std::memory_order_relaxed) & (MaxThreads - 1);
        }

        return id;
    }

    template<typename F>
    void for_each_bit(uint64_t mask, F&& func) {
        while (mask > 0) {
            func(std::countr_zero(mask));
            mask &= (mask - 1);
        }
    }

    void process_buffer(int buf_idx) {
        UpdateOp op;
        auto& buffer = _update_buffers[buf_idx];

        while (buffer.pop(op)) {
            sizes::prefetch(&_collection.get_meta(_collection.get_head()), 1);

            if (_collection.is_valid_gen(op.idx, op.gen)) {
                _collection.move_to_front(op.idx);
            }
        }
    }

    void apply_updates() {
        uint64_t mask = _dirty_mask.exchange(0, std::memory_order_acquire);
        
        for_each_bit(mask, [this](int buf_idx) {
            process_buffer(buf_idx);
        });

        if (!_retired_list.empty()) [[likely]] {
            this->cleanup_retired();
        }
    }

    void mark_access(cacheMap::index_type idx, uint32_t gen) noexcept {
        const auto tid = get_thread_id();

        if (tid == std::numeric_limits<std::size_t>::max()) [[unlikely]] return;

        if (_update_buffers[tid].push({idx, gen})) [[likely]] {
            const uint64_t mask = 1ULL << tid;
            if (!(_dirty_mask.load(std::memory_order_relaxed) & mask)) {    // Test
                _dirty_mask.fetch_or(mask, std::memory_order_release);      // Test & Set bit in mask
            }
        }
    }

    void cleanup_retired() {
        uint64_t min_e = this->get_min_active();
        std::erase_if(_retired_list, [min_e](auto& obj) {
            return obj.epoch < min_e;
        });
    }

    static void spin_wait(std::atomic_flag& lock) noexcept {
        uint32_t spin_count = 0;
        constexpr uint32_t MAX_SPIN = 2048;

        while (lock.test_and_set(std::memory_order_acquire)) {
            if (++spin_count < MAX_SPIN) [[likely]] {
                __builtin_ia32_pause(); 
            } else {
                std::this_thread::yield();
                spin_count = 0;
            }
        }
    }

    static void release_lock(std::atomic_flag& lock) noexcept {
        lock.clear(std::memory_order_release);
    }

     //Insert or update path (eviction is included)
     //CRITICAL section!
    void commit_put(const KeyType& key, std::shared_ptr<ValueType>&& new_ptr) noexcept {
        auto final_res = _collection.lookup(key);

        if (final_res.ptr) [[likely]] {
            // Update
            auto old = _collection.update_slot(final_res.idx, std::move(new_ptr));
            _retired_list.push_back({std::move(old), this->current_epoch()});
        } else {
            // Insert
            if (_collection.size() >= Capacity) [[unlikely]] {
                auto tail_idx = _collection.get_tail();
                auto evicted_ptr = _collection.get_data(tail_idx).value;

                _retired_list.push_back({std::move(evicted_ptr), this->current_epoch()});
                _collection.erase_index(tail_idx);
                final_res.idx = _collection.assign_slot(key);
            }

            _collection.emplace_at(final_res.idx, key, std::move(new_ptr));
        }

        _collection.move_to_front(final_res.idx);
    }

public:

    std::shared_ptr<ValueType> get(const KeyType& key) noexcept {
        const auto tid = get_thread_id();
        auto guard = this->enter_epoch(tid);

        // shared_ptr copied
        auto res = _collection.get_lockless(key); //NOTE it needs to prove
        if (!res.ptr) [[unlikely]] return nullptr;

        mark_access(res.idx, res.gen);
        sizes::prefetch(res.ptr.get(), 0);

        return std::move(res.ptr);
    }

    template <typename T>
    void put(const KeyType& key, T&& value) {

        //CRITICAL  ****    Potential problem if user calls yield
        //NOTE      ****    Use mutex, instead of spin
        //std::lock_guard<std::mutex> lock(_mtx);
        spin_wait(_spin_lock);
            auto res = _collection.lookup(key);

            if (res.ptr) [[likely]] { // Quiet Update
                if (*res.ptr == value) [[likely]] {
                    _collection.move_to_front(res.idx);
                     release_lock(_spin_lock);
                    return;
                }
            }
        release_lock(_spin_lock);

//        auto new_ptr = std::make_shared<ValueType>(std::forward<T>(value));
        auto new_ptr = std::allocate_shared<ValueType>(
            HugePagesAllocator<ValueType>{},
            std::forward<T>(value)
        );

        spin_wait(_spin_lock);
            this->bump_epoch();

            if (_dirty_mask.load(std::memory_order_relaxed)) {
                apply_updates();
            }

            commit_put(key, std::move(new_ptr));

            if (_retired_list.size() >= 64) {
                this->cleanup_retired();
            }

        release_lock(_spin_lock);
    }

private:
    alignas(CacheLine) PaddedSPSC               _update_buffers[MaxThreads];
    alignas(CacheLine) std::atomic<uint64_t>    _dirty_mask{0};
    std::vector<RetiredObject> _retired_list;

    cacheMap            _collection;

    //CRITICAL  ****    Potential problem if user calls yield
    //NOTE      ****    Use mutex, instead of spin
    std::atomic_flag    _spin_lock = ATOMIC_FLAG_INIT;
    //std::mutex          _mtx;
};

//  Wrapper for SharedLRU
template <template<typename, typename, std::size_t> class CacheImpl,
    typename KeyType, typename ValueType,
    std::size_t TotalCapacity = 2 * 1024,
    std::size_t ShardsCount = 16>
requires PowerOfTwoValue<ShardsCount>
class Lv4_ShardedCache : private NonCopyableNonMoveable {
    static constexpr std::size_t CacheLine = sizes::CacheLine;
    static constexpr std::size_t ShardCapacity = TotalCapacity / ShardsCount;
    static_assert(ShardCapacity >= 64, "Shard capacity too small!");
    using Cache = CacheImpl<KeyType, ValueType, ShardCapacity>;

public:
    static constexpr std::string name() noexcept {
        return "Lv3_Sharded<" + std::string(Cache::name()) + ">";
    }

    using value_type = ValueType;
    using key_type = KeyType;

private:
    static constexpr std::size_t Mask = ShardsCount - 1;
    static_assert(TotalCapacity > 0, "TotalCapacity must be > 0");
    static_assert(ShardsCount > 0, "ShardsCount must be > 0");
    static_assert(std::has_single_bit(ShardsCount), "ShardsCount must be power of 2");
    static_assert(std::has_single_bit(alignof(Cache)), "Alignment must be power of 2");

    std::size_t get_shard_idx(const KeyType& key) const noexcept {
        return std::hash<KeyType>{}(key) & Mask;
    }

public:
    Lv4_ShardedCache() {
        _shards.reserve(ShardsCount);
        for (std::size_t i = 0; i < ShardsCount; ++i) {
            _shards.emplace_back(std::make_unique<Cache>());
        }
    }

    std::shared_ptr<ValueType> get(const KeyType& key) noexcept {
        return _shards[get_shard_idx(key)].cache->get(key);
    }

    template <typename T>
    void put(const KeyType& key, T&& value) {
        _shards[get_shard_idx(key)].cache->put(key, std::forward<T>(value));
    }

private:
    struct alignas(CacheLine) ShardWrapper {
        std::unique_ptr<Cache> cache;
        explicit ShardWrapper(std::unique_ptr<Cache> c) : cache(std::move(c)) {}
    };
    std::vector<ShardWrapper> _shards;
};
