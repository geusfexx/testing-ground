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


================================================
SCENARIO: Readers(28) Writers(4)
	| NORMAL MODE |
CacheSize: 4096 KeyRange: 4915
================================================

Testing: Sharded<Lvl2_SPSCBuffer_DeferredFlatLRU>...
Time: 0.268144 s 
Ops/sec: 119.339 M
Misses: 26094577

Done: 32 threads finished.


 Performance counter stats for './lv2.out':

          1,593.97 msec task-clock                       #    5.859 CPUs utilized          
             1,786      context-switches                 #    1.120 K/sec                  
                50      cpu-migrations                   #   31.368 /sec                   
               390      page-faults                      #  244.671 /sec                   
     4,798,117,632      cycles                           #    3.010 GHz                      (81.70%)
       100,031,351      stalled-cycles-frontend          #    2.08% frontend cycles idle     (80.44%)
        87,000,005      stalled-cycles-backend           #    1.81% backend cycles idle      (82.18%)
     3,716,567,840      instructions                     #    0.77  insn per cycle         
                                                  #    0.03  stalled cycles per insn  (84.61%)
       814,672,322      branches                         #  511.095 M/sec                    (86.44%)
        16,850,306      branch-misses                    #    2.07% of all branches          (86.92%)

       0.272033689 seconds time elapsed

       1.323849000 seconds user
       0.257415000 seconds sys




================================================
SCENARIO: Readers(28) Writers(4)
	| NORMAL MODE |
CacheSize: 4096 KeyRange: 4915
================================================

Testing: Sharded<Lv3_SPSCBuffer_DeferredFlatLRU>...
Time: 0.348663 s 
Ops/sec: 91.7792 M
Misses: 25893160

Done: 32 threads finished.


 Performance counter stats for './lv3.out':

          1,970.85 msec task-clock                       #    5.593 CPUs utilized          
             3,208      context-switches                 #    1.628 K/sec                  
                50      cpu-migrations                   #   25.370 /sec                   
               391      page-faults                      #  198.391 /sec                   
     5,728,894,643      cycles                           #    2.907 GHz                      (80.40%)
       146,108,755      stalled-cycles-frontend          #    2.55% frontend cycles idle     (83.70%)
       129,413,101      stalled-cycles-backend           #    2.26% backend cycles idle      (83.31%)
     3,853,240,585      instructions                     #    0.67  insn per cycle         
                                                  #    0.04  stalled cycles per insn  (86.43%)
       828,111,715      branches                         #  420.180 M/sec                    (87.27%)
        21,141,548      branch-misses                    #    2.55% of all branches          (80.97%)

       0.352354029 seconds time elapsed

       1.589052000 seconds user
       0.358096000 seconds sys


========================================================
SCENARIO: Readers(28) Writers(4)                        
                   | NORMAL MODE |
--------------------------------------------------------
  CacheSize:         65536   KeyRange:            78643
  Payload Size:        128   Shards amount:          32
========================================================

Testing: Sharded<Lv3_SPSCBuffer_DeferredFlatLRU>...
Time: 285.28 s 
Ops/sec: 112.171 M
Avg Latency: 8.915 ns
Misses: 18610193577

Testing: Sharded<Lv4_SPSCBuffer_DeferredFlatLRU>...
Time: 117.815 s 
Ops/sec: 271.612 M
Avg Latency: 3.68172 ns
Misses: 445118555

Testing: Lv2_Sharded<Lv4_SPSCBuffer_DeferredFlatLRU>...
Time: 116.336 s 
Ops/sec: 275.066 M
Avg Latency: 3.63549 ns
Misses: 549225535

Done: 32 threads finished.




========================================================
SCENARIO: Readers(28) Writers(4)                        
                   | NORMAL MODE |
--------------------------------------------------------
  CacheSize:         65536   KeyRange:            78643
  Payload Size:        128   Shards amount:          32
========================================================

Testing: Sharded<Lv3_SPSCBuffer_DeferredFlatLRU>...
Time: 294.514 s 
Ops/sec: 108.654 M
Avg Latency: 9.20357 ns
Misses: 18839276330

Done: 32 threads finished.


 Performance counter stats for './lv3.out':

      1,972,081.43 msec task-clock                       #    6.668 CPUs utilized          
         1,630,410      context-switches                 #  826.746 /sec                   
             7,792      cpu-migrations                   #    3.951 /sec                   
             6,552      page-faults                      #    3.322 /sec                   
 5,899,129,214,761      cycles                           #    2.991 GHz                      (83.36%)
   127,097,965,774      stalled-cycles-frontend          #    2.15% frontend cycles idle     (83.31%)
    94,116,059,520      stalled-cycles-backend           #    1.60% backend cycles idle      (83.33%)
 4,137,625,662,097      instructions                     #    0.70  insn per cycle         
                                                  #    0.03  stalled cycles per insn  (83.31%)
   831,137,827,687      branches                         #  421.452 M/sec                    (83.32%)
    17,767,918,274      branch-misses                    #    2.14% of all branches          (83.36%)

     295.742670007 seconds time elapsed

    1757.282337000 seconds user
     205.382546000 seconds sys



========================================================
SCENARIO: Readers(28) Writers(4)                        
                   | NORMAL MODE |
--------------------------------------------------------
  CacheSize:         65536   KeyRange:            78643
  Payload Size:        128   Shards amount:          32
========================================================

Testing: Sharded<Lv4_SPSCBuffer_DeferredFlatLRU>...
Time: 117.628 s 
Ops/sec: 272.043 M
Avg Latency: 3.67589 ns
Misses: 363762437

Done: 32 threads finished.


 Performance counter stats for './lv4.out':

        630,042.36 msec task-clock                       #    5.301 CPUs utilized          
           648,446      context-switches                 #    1.029 K/sec                  
               206      cpu-migrations                   #    0.327 /sec                   
             6,553      page-faults                      #   10.401 /sec                   
 1,877,721,717,069      cycles                           #    2.980 GHz                      (83.41%)
    86,523,818,809      stalled-cycles-frontend          #    4.61% frontend cycles idle     (83.25%)
    54,817,491,439      stalled-cycles-backend           #    2.92% backend cycles idle      (83.42%)
 2,478,025,960,439      instructions                     #    1.32  insn per cycle         
                                                  #    0.03  stalled cycles per insn  (83.34%)
   441,721,963,069      branches                         #  701.099 M/sec                    (83.34%)
     7,760,701,407      branch-misses                    #    1.76% of all branches          (83.24%)

     118.857790732 seconds time elapsed

     468.925411000 seconds user
     154.822412000 seconds sys



========================================================
SCENARIO: Readers(28) Writers(4)                        
                   | NORMAL MODE |
--------------------------------------------------------
  CacheSize:         65536   KeyRange:            78643
  Payload Size:        128   Shards amount:          32
========================================================

Testing: Lv2_Sharded<Lv4_SPSCBuffer_DeferredFlatLRU>...
Time: 119.155 s 
Ops/sec: 268.558 M
Avg Latency: 3.72359 ns
Misses: 313389133

Done: 32 threads finished.


 Performance counter stats for './s2lv4.out':

        640,316.83 msec task-clock                       #    5.319 CPUs utilized          
           651,541      context-switches                 #    1.018 K/sec                  
               191      cpu-migrations                   #    0.298 /sec                   
             6,553      page-faults                      #   10.234 /sec                   
 1,907,279,801,958      cycles                           #    2.979 GHz                      (83.35%)
    89,445,110,652      stalled-cycles-frontend          #    4.69% frontend cycles idle     (83.34%)
    55,459,202,811      stalled-cycles-backend           #    2.91% backend cycles idle      (83.33%)
 2,507,939,824,595      instructions                     #    1.31  insn per cycle         
                                                  #    0.04  stalled cycles per insn  (83.25%)
   440,767,007,004      branches                         #  688.358 M/sec                    (83.40%)
     7,852,246,626      branch-misses                    #    1.78% of all branches          (83.34%)

     120.390799739 seconds time elapsed

     475.598817000 seconds user
     158.231692000 seconds sys

