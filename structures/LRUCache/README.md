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
  Payload Size:        128   Shards amount:          32
========================================================

Testing: Lv2_Sharded<Lv4_SPSCBuffer_DeferredFlatLRU>...
Time: 25.5513 s 
Ops/sec: 125.238 M
Avg Latency: 7.98479 ns
Misses: 0.000 (0.00%)

Testing: Lv3_Sharded<Lv5_SPSCBuffer_DeferredFlatLRU>...
Time: 23.20 s 
Ops/sec: 137.959 M
Avg Latency: 7.25 ns
Misses: 0.000 (0.00%)

Done: 32 threads finished.

========================================================
SCENARIO: Readers(28) Writers(4) Iterations: 100 M
     
                   | NORMAL MODE |
--------------------------------------------------------
  CacheSize:         65536   KeyRange:            64880
  Payload Size:      65536   Shards amount:          32
========================================================

Testing: Lv2_Sharded<Lv4_SPSCBuffer_DeferredFlatLRU>...
Time: 47.8037 s 
Ops/sec: 66.940 M
Avg Latency: 14.9386 ns
Misses: 0.000 (0.00%)

Testing: Lv3_Sharded<Lv5_SPSCBuffer_DeferredFlatLRU>...
Time: 26.37 s 
Ops/sec: 121.337 M
Avg Latency: 8.24 ns
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
Time: 31.52 s 
Ops/sec: 101.523 M
Avg Latency: 9.85 ns
Misses: 474.469 M (16.95%)

Testing: Lv3_Sharded<Lv5_SPSCBuffer_DeferredFlatLRU>...
Time: 31.97 s 
Ops/sec: 100.091 M
Avg Latency: 9.99 ns
Misses: 528.029 M (18.86%)

Done: 32 threads finished.

========================================================
SCENARIO: Readers(28) Writers(4) Iterations: 100 M
     
                   | NORMAL MODE |
--------------------------------------------------------
  CacheSize:         65536   KeyRange:            78643
  Payload Size:      65536   Shards amount:          32
========================================================

Testing: Lv2_Sharded<Lv4_SPSCBuffer_DeferredFlatLRU>...
Time: 199.907 s 
Ops/sec: 16.007 M
Avg Latency: 62.471 ns
Misses: 480.243 M (17.15%)

Testing: Lv3_Sharded<Lv5_SPSCBuffer_DeferredFlatLRU>...
Time: 135.76 s 
Ops/sec: 23.570 M
Avg Latency: 42.43 ns
Misses: 470.611 M (16.81%)

Done: 32 threads finished.
