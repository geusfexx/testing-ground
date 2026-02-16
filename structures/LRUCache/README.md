perf log with minimum perf/OS distorsion near the same clear execution:

Preconditions:
    - Virtual machine (8 vCPUs) based on an 8C/16T physical processor.
    - Threads oversubscription 4:1.

Summary:
    Lv2 (119.3 Mops) [95-127]: Wins due to the "cheapness" of list operations (std::list::splice) with a ready-made iterator.
    Lv3 (91.8 Mops) [89-107]: Loses ~20-25% in raw speed. This is due to the instruction penalty.
    Context Switches (1.7k vs. 3.2k) - Just a propagation of uncertainty due to OS/planner noise
    Cycles (4.8 billion vs. 5.7 billion) - It looks like point to improve (double hashing, heavy apply stategy)
    5% difference of branch misses rate - secondary point.

========================================================
SCENARIO: Readers(28) Writers(4) Iterations: 100 M
     
                   | NORMAL MODE |
--------------------------------------------------------
  CacheSize:         65536   KeyRange:            64880
  Payload Size:      65536   Shards amount:          32
========================================================

Testing: Lv2_Sharded<Lv4_SPSCBuffer_DeferredFlatLRU>...
Time: 47.2467 s 
Ops/sec: 67.730 M
Avg Latency: 14.7646 ns
Misses: 0.000 (0.00%)

Testing: Lv3_Sharded<Lv5_SPSCBuffer_DeferredFlatLRU>...
Time: 37.68 s 
Ops/sec: 84.923 M
Avg Latency: 11.78 ns
Misses: 0.000 (0.00%)

Done: 32 threads finished.

========================================================
SCENARIO: Readers(28) Writers(4) Iterations: 100 M
     
                   | NORMAL MODE |
--------------------------------------------------------
  CacheSize:         65536   KeyRange:            64880
  Payload Size:        128   Shards amount:          32
========================================================

Testing: Lv2_Sharded<Lv4_SPSCBuffer_DeferredFlatLRU>...
Time: 25.5112 s 
Ops/sec: 125.435 M
Avg Latency: 7.97224 ns
Misses: 0.000 (0.00%)

Testing: Lv3_Sharded<Lv5_SPSCBuffer_DeferredFlatLRU>...
Time: 34.62 s 
Ops/sec: 92.421 M
Avg Latency: 10.82 ns
Misses: 0.000 (0.00%)

Done: 32 threads finished.

========================================================
SCENARIO: Readers(28) Writers(4) Iterations: 100 M
     
                   | NORMAL MODE |
--------------------------------------------------------
  CacheSize:         65536   KeyRange:            78643
  Payload Size:        128   Shards amount:          32
========================================================

Testing: Lv2_Sharded<Lv4_SPSCBuffer_DeferredFlatLRU>...
Time: 31.0576 s 
Ops/sec: 103.034 M
Avg Latency: 9.70551 ns
Misses: 467.225 M (16.69%)

Testing: Lv3_Sharded<Lv5_SPSCBuffer_DeferredFlatLRU>...
Time: 52.30 s 
Ops/sec: 61.185 M
Avg Latency: 16.34 ns
Misses: 585.394 M (20.91%)

Done: 32 threads finished.

========================================================
SCENARIO: Readers(28) Writers(4) Iterations: 100 M
     
                   | NORMAL MODE |
--------------------------------------------------------
  CacheSize:         65536   KeyRange:            78643
  Payload Size:      65536   Shards amount:          32
========================================================

Testing: Lv2_Sharded<Lv4_SPSCBuffer_DeferredFlatLRU>...
Time: 200.947 s 
Ops/sec: 15.925 M
Avg Latency: 62.7959 ns
Misses: 469.078 M (16.75%)

Testing: Lv3_Sharded<Lv5_SPSCBuffer_DeferredFlatLRU>...
Time: 177.21 s 
Ops/sec: 18.057 M
Avg Latency: 55.38 ns
Misses: 532.082 M (19.00%)

Done: 32 threads finished.

