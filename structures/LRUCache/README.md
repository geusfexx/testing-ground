pref log with minimum perf/OS distorsion near the same clear execution:

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
