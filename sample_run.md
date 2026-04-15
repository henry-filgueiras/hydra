── Building hydra_bench (Release) ──────────────────────────────────────
--   Boost found   : /opt/homebrew/include
-- ── Hydra build configuration ──────────────────────
--   Boost bench  : ON
[ 90%] Built target benchmark
[100%] Built target hydra_bench

── Running benchmarks → /Users/henry/hydra/build-rel/bench_results.json ────────────────────────────────
-----------------------------------------------------------------------------------
Benchmark                                         Time             CPU   Iterations
-----------------------------------------------------------------------------------
baseline/u64_add                               2.48 ns         2.47 ns    112508539
baseline/u64_add                               2.47 ns         2.47 ns    112508539
baseline/u64_add                               2.47 ns         2.47 ns    112508539
baseline/u64_add_mean                          2.47 ns         2.47 ns            3
baseline/u64_add_median                        2.47 ns         2.47 ns            3
baseline/u64_add_stddev                       0.005 ns        0.005 ns            3
baseline/u64_add_cv                            0.19 %          0.20 %             3
baseline/u64_mul                               3.49 ns         3.49 ns     78709170
baseline/u64_mul                               3.34 ns         3.34 ns     78709170
baseline/u64_mul                               3.40 ns         3.40 ns     78709170
baseline/u64_mul_mean                          3.41 ns         3.41 ns            3
baseline/u64_mul_median                        3.40 ns         3.40 ns            3
baseline/u64_mul_stddev                       0.074 ns        0.074 ns            3
baseline/u64_mul_cv                            2.17 %          2.17 %             3
hydra/small_add                                3.15 ns         3.14 ns     84615152
hydra/small_add                                3.14 ns         3.14 ns     84615152
hydra/small_add                                3.11 ns         3.11 ns     84615152
hydra/small_add_mean                           3.13 ns         3.13 ns            3
hydra/small_add_median                         3.14 ns         3.14 ns            3
hydra/small_add_stddev                        0.021 ns        0.021 ns            3
hydra/small_add_cv                             0.66 %          0.67 %             3
hydra/small_mul                                4.02 ns         4.02 ns     69262356
hydra/small_mul                                4.03 ns         4.03 ns     69262356
hydra/small_mul                                4.03 ns         4.03 ns     69262356
hydra/small_mul_mean                           4.03 ns         4.03 ns            3
hydra/small_mul_median                         4.03 ns         4.03 ns            3
hydra/small_mul_stddev                        0.007 ns        0.007 ns            3
hydra/small_mul_cv                             0.17 %          0.17 %             3
hydra/small_sub                                1.09 ns         1.09 ns    250680418
hydra/small_sub                                1.09 ns         1.09 ns    250680418
hydra/small_sub                                1.10 ns         1.09 ns    250680418
hydra/small_sub_mean                           1.09 ns         1.09 ns            3
hydra/small_sub_median                         1.09 ns         1.09 ns            3
hydra/small_sub_stddev                        0.004 ns        0.004 ns            3
hydra/small_sub_cv                             0.35 %          0.35 %             3
hydra/widening_add                             3.17 ns         3.16 ns     87774295
hydra/widening_add                             3.18 ns         3.18 ns     87774295
hydra/widening_add                             3.18 ns         3.18 ns     87774295
hydra/widening_add_mean                        3.17 ns         3.17 ns            3
hydra/widening_add_median                      3.18 ns         3.18 ns            3
hydra/widening_add_stddev                     0.007 ns        0.007 ns            3
hydra/widening_add_cv                          0.21 %          0.21 %             3
hydra/widening_mul_128                         3.93 ns         3.93 ns     73154801
hydra/widening_mul_128                         3.85 ns         3.84 ns     73154801
hydra/widening_mul_128                         3.80 ns         3.79 ns     73154801
hydra/widening_mul_128_mean                    3.86 ns         3.86 ns            3
hydra/widening_mul_128_median                  3.85 ns         3.84 ns            3
hydra/widening_mul_128_stddev                 0.068 ns        0.068 ns            3
hydra/widening_mul_128_cv                      1.75 %          1.75 %             3
hydra/medium_add                               6.42 ns         6.42 ns     42921744
hydra/medium_add                               6.43 ns         6.43 ns     42921744
hydra/medium_add                               6.35 ns         6.34 ns     42921744
hydra/medium_add_mean                          6.40 ns         6.40 ns            3
hydra/medium_add_median                        6.42 ns         6.42 ns            3
hydra/medium_add_stddev                       0.044 ns        0.044 ns            3
hydra/medium_add_cv                            0.69 %          0.68 %             3
hydra/medium_mul                               18.5 ns         18.5 ns     14797512
hydra/medium_mul                               18.5 ns         18.5 ns     14797512
hydra/medium_mul                               18.2 ns         18.2 ns     14797512
hydra/medium_mul_mean                          18.4 ns         18.4 ns            3
hydra/medium_mul_median                        18.5 ns         18.5 ns            3
hydra/medium_mul_stddev                       0.159 ns        0.160 ns            3
hydra/medium_mul_cv                            0.86 %          0.87 %             3
alloc/largerep_create_destroy/4                10.1 ns         10.1 ns     28411974 items_per_second=98.9644M/s
alloc/largerep_create_destroy/4                10.0 ns         10.0 ns     28411974 items_per_second=99.6492M/s
alloc/largerep_create_destroy/4                10.0 ns         10.0 ns     28411974 items_per_second=99.7597M/s
alloc/largerep_create_destroy/4_mean           10.1 ns         10.1 ns            3 items_per_second=99.4578M/s
alloc/largerep_create_destroy/4_median         10.0 ns         10.0 ns            3 items_per_second=99.6492M/s
alloc/largerep_create_destroy/4_stddev        0.049 ns        0.044 ns            3 items_per_second=430.861k/s
alloc/largerep_create_destroy/4_cv             0.48 %          0.43 %             3 items_per_second=0.43%
alloc/largerep_create_destroy/16               8.67 ns         8.66 ns     32703403 items_per_second=115.482M/s
alloc/largerep_create_destroy/16               8.57 ns         8.56 ns     32703403 items_per_second=116.818M/s
alloc/largerep_create_destroy/16               8.57 ns         8.56 ns     32703403 items_per_second=116.773M/s
alloc/largerep_create_destroy/16_mean          8.60 ns         8.59 ns            3 items_per_second=116.358M/s
alloc/largerep_create_destroy/16_median        8.57 ns         8.56 ns            3 items_per_second=116.773M/s
alloc/largerep_create_destroy/16_stddev       0.056 ns        0.056 ns            3 items_per_second=758.622k/s
alloc/largerep_create_destroy/16_cv            0.65 %          0.65 %             3 items_per_second=0.65%
alloc/largerep_create_destroy/64               16.1 ns         16.1 ns     17428218 items_per_second=62.1254M/s
alloc/largerep_create_destroy/64               16.1 ns         16.1 ns     17428218 items_per_second=62.2065M/s
alloc/largerep_create_destroy/64               15.9 ns         15.9 ns     17428218 items_per_second=62.747M/s
alloc/largerep_create_destroy/64_mean          16.0 ns         16.0 ns            3 items_per_second=62.3596M/s
alloc/largerep_create_destroy/64_median        16.1 ns         16.1 ns            3 items_per_second=62.2065M/s
alloc/largerep_create_destroy/64_stddev       0.086 ns        0.087 ns            3 items_per_second=337.887k/s
alloc/largerep_create_destroy/64_cv            0.54 %          0.54 %             3 items_per_second=0.54%
alloc/largerep_create_destroy/256              13.3 ns         13.3 ns     21149314 items_per_second=75.1877M/s
alloc/largerep_create_destroy/256              13.2 ns         13.2 ns     21149314 items_per_second=75.657M/s
alloc/largerep_create_destroy/256              13.2 ns         13.2 ns     21149314 items_per_second=75.9857M/s
alloc/largerep_create_destroy/256_mean         13.2 ns         13.2 ns            3 items_per_second=75.6101M/s
alloc/largerep_create_destroy/256_median       13.2 ns         13.2 ns            3 items_per_second=75.657M/s
alloc/largerep_create_destroy/256_stddev      0.070 ns        0.070 ns            3 items_per_second=401.053k/s
alloc/largerep_create_destroy/256_cv           0.53 %          0.53 %             3 items_per_second=0.53%
alloc/from_limbs/4                             12.0 ns         12.0 ns     22867970 items_per_second=83.4305M/s
alloc/from_limbs/4                             12.0 ns         12.0 ns     22867970 items_per_second=83.5659M/s
alloc/from_limbs/4                             12.0 ns         12.0 ns     22867970 items_per_second=83.1853M/s
alloc/from_limbs/4_mean                        12.0 ns         12.0 ns            3 items_per_second=83.3939M/s
alloc/from_limbs/4_median                      12.0 ns         12.0 ns            3 items_per_second=83.4305M/s
alloc/from_limbs/4_stddev                     0.028 ns        0.028 ns            3 items_per_second=192.917k/s
alloc/from_limbs/4_cv                          0.23 %          0.23 %             3 items_per_second=0.23%
alloc/from_limbs/8                             10.1 ns         10.1 ns     28100318 items_per_second=99.0236M/s
alloc/from_limbs/8                             10.4 ns         10.4 ns     28100318 items_per_second=96.6078M/s
alloc/from_limbs/8                             9.97 ns         9.96 ns     28100318 items_per_second=100.356M/s
alloc/from_limbs/8_mean                        10.1 ns         10.1 ns            3 items_per_second=98.6624M/s
alloc/from_limbs/8_median                      10.1 ns         10.1 ns            3 items_per_second=99.0236M/s
alloc/from_limbs/8_stddev                     0.197 ns        0.196 ns            3 items_per_second=1.8999M/s
alloc/from_limbs/8_cv                          1.94 %          1.94 %             3 items_per_second=1.93%
alloc/from_limbs/16                            10.3 ns         10.3 ns     27271576 items_per_second=97.2086M/s
alloc/from_limbs/16                            10.2 ns         10.2 ns     27271576 items_per_second=97.687M/s
alloc/from_limbs/16                            10.3 ns         10.2 ns     27271576 items_per_second=97.6269M/s
alloc/from_limbs/16_mean                       10.3 ns         10.3 ns            3 items_per_second=97.5075M/s
alloc/from_limbs/16_median                     10.3 ns         10.2 ns            3 items_per_second=97.6269M/s
alloc/from_limbs/16_stddev                    0.027 ns        0.027 ns            3 items_per_second=260.599k/s
alloc/from_limbs/16_cv                         0.26 %          0.27 %             3 items_per_second=0.27%
alloc/from_limbs/64                            20.6 ns         20.6 ns     13587616 items_per_second=48.4701M/s
alloc/from_limbs/64                            20.9 ns         20.9 ns     13587616 items_per_second=47.8469M/s
alloc/from_limbs/64                            20.6 ns         20.6 ns     13587616 items_per_second=48.5756M/s
alloc/from_limbs/64_mean                       20.7 ns         20.7 ns            3 items_per_second=48.2975M/s
alloc/from_limbs/64_median                     20.6 ns         20.6 ns            3 items_per_second=48.4701M/s
alloc/from_limbs/64_stddev                    0.168 ns        0.170 ns            3 items_per_second=393.793k/s
alloc/from_limbs/64_cv                         0.81 %          0.82 %             3 items_per_second=0.82%
alloc/from_limbs/256                           29.2 ns         29.2 ns      9656171 items_per_second=34.2415M/s
alloc/from_limbs/256                           29.0 ns         29.0 ns      9656171 items_per_second=34.4836M/s
alloc/from_limbs/256                           28.9 ns         28.9 ns      9656171 items_per_second=34.6533M/s
alloc/from_limbs/256_mean                      29.0 ns         29.0 ns            3 items_per_second=34.4595M/s
alloc/from_limbs/256_median                    29.0 ns         29.0 ns            3 items_per_second=34.4836M/s
alloc/from_limbs/256_stddev                   0.172 ns        0.174 ns            3 items_per_second=206.95k/s
alloc/from_limbs/256_cv                        0.59 %          0.60 %             3 items_per_second=0.60%
alloc/largerep_clone/4                         11.9 ns         11.9 ns     23358833 items_per_second=83.7934M/s
alloc/largerep_clone/4                         11.9 ns         11.9 ns     23358833 items_per_second=84.0587M/s
alloc/largerep_clone/4                         12.1 ns         12.1 ns     23358833 items_per_second=82.4704M/s
alloc/largerep_clone/4_mean                    12.0 ns         12.0 ns            3 items_per_second=83.4408M/s
alloc/largerep_clone/4_median                  11.9 ns         11.9 ns            3 items_per_second=83.7934M/s
alloc/largerep_clone/4_stddev                 0.123 ns        0.123 ns            3 items_per_second=850.841k/s
alloc/largerep_clone/4_cv                      1.03 %          1.03 %             3 items_per_second=1.02%
alloc/largerep_clone/16                        9.99 ns         9.98 ns     27881226 items_per_second=100.166M/s
alloc/largerep_clone/16                        10.0 ns         10.0 ns     27881226 items_per_second=99.9836M/s
alloc/largerep_clone/16                        10.0 ns         10.0 ns     27881226 items_per_second=99.6043M/s
alloc/largerep_clone/16_mean                   10.0 ns         10.0 ns            3 items_per_second=99.9181M/s
alloc/largerep_clone/16_median                 10.0 ns         10.0 ns            3 items_per_second=99.9836M/s
alloc/largerep_clone/16_stddev                0.028 ns        0.029 ns            3 items_per_second=286.749k/s
alloc/largerep_clone/16_cv                     0.28 %          0.29 %             3 items_per_second=0.29%
alloc/largerep_clone/64                        19.3 ns         19.3 ns     14537374 items_per_second=51.8799M/s
alloc/largerep_clone/64                        19.3 ns         19.3 ns     14537374 items_per_second=51.9144M/s
alloc/largerep_clone/64                        19.3 ns         19.3 ns     14537374 items_per_second=51.8351M/s
alloc/largerep_clone/64_mean                   19.3 ns         19.3 ns            3 items_per_second=51.8765M/s
alloc/largerep_clone/64_median                 19.3 ns         19.3 ns            3 items_per_second=51.8799M/s
alloc/largerep_clone/64_stddev                0.015 ns        0.015 ns            3 items_per_second=39.7248k/s
alloc/largerep_clone/64_cv                     0.08 %          0.08 %             3 items_per_second=0.08%
alloc/largerep_clone/256                       27.8 ns         27.7 ns     10098096 items_per_second=36.0412M/s
alloc/largerep_clone/256                       27.6 ns         27.6 ns     10098096 items_per_second=36.2653M/s
alloc/largerep_clone/256                       27.8 ns         27.8 ns     10098096 items_per_second=36.0271M/s
alloc/largerep_clone/256_mean                  27.7 ns         27.7 ns            3 items_per_second=36.1112M/s
alloc/largerep_clone/256_median                27.8 ns         27.7 ns            3 items_per_second=36.0412M/s
alloc/largerep_clone/256_stddev               0.102 ns        0.102 ns            3 items_per_second=133.626k/s
alloc/largerep_clone/256_cv                    0.37 %          0.37 %             3 items_per_second=0.37%
alloc/normalize_large_to_medium                13.8 ns         13.8 ns     19917485
alloc/normalize_large_to_medium                13.9 ns         13.9 ns     19917485
alloc/normalize_large_to_medium                14.2 ns         14.2 ns     19917485
alloc/normalize_large_to_medium_mean           14.0 ns         14.0 ns            3
alloc/normalize_large_to_medium_median         13.9 ns         13.9 ns            3
alloc/normalize_large_to_medium_stddev        0.215 ns        0.215 ns            3
alloc/normalize_large_to_medium_cv             1.54 %          1.54 %             3
alloc/normalize_medium_to_small                2.04 ns         2.04 ns    115245308
alloc/normalize_medium_to_small                2.01 ns         2.01 ns    115245308
alloc/normalize_medium_to_small                2.01 ns         2.01 ns    115245308
alloc/normalize_medium_to_small_mean           2.02 ns         2.02 ns            3
alloc/normalize_medium_to_small_median         2.01 ns         2.01 ns            3
alloc/normalize_medium_to_small_stddev        0.020 ns        0.020 ns            3
alloc/normalize_medium_to_small_cv             0.97 %          0.97 %             3
copy/small                                    0.336 ns        0.336 ns    767123288
copy/small                                    0.334 ns        0.333 ns    767123288
copy/small                                    0.336 ns        0.336 ns    767123288
copy/small_mean                               0.335 ns        0.335 ns            3
copy/small_median                             0.336 ns        0.336 ns            3
copy/small_stddev                             0.001 ns        0.001 ns            3
copy/small_cv                                  0.39 %          0.39 %             3
copy/medium                                   0.429 ns        0.429 ns    618579476
copy/medium                                   0.438 ns        0.437 ns    618579476
copy/medium                                   0.436 ns        0.436 ns    618579476
copy/medium_mean                              0.434 ns        0.434 ns            3
copy/medium_median                            0.436 ns        0.436 ns            3
copy/medium_stddev                            0.004 ns        0.004 ns            3
copy/medium_cv                                 1.01 %          1.00 %             3
copy/large/4                                   12.3 ns         12.3 ns     22491766 bytes_per_second=2.42889Gi/s
copy/large/4                                   12.5 ns         12.5 ns     22491766 bytes_per_second=2.37922Gi/s
copy/large/4                                   12.4 ns         12.4 ns     22491766 bytes_per_second=2.39918Gi/s
copy/large/4_mean                              12.4 ns         12.4 ns            3 bytes_per_second=2.40243Gi/s
copy/large/4_median                            12.4 ns         12.4 ns            3 bytes_per_second=2.39918Gi/s
copy/large/4_stddev                           0.129 ns        0.129 ns            3 bytes_per_second=25.592Mi/s
copy/large/4_cv                                1.04 %          1.04 %             3 bytes_per_second=1.04%
copy/large/16                                  10.4 ns         10.4 ns     27030158 bytes_per_second=11.4731Gi/s
copy/large/16                                  10.5 ns         10.5 ns     27030158 bytes_per_second=11.3605Gi/s
copy/large/16                                  10.5 ns         10.5 ns     27030158 bytes_per_second=11.3186Gi/s
copy/large/16_mean                             10.5 ns         10.5 ns            3 bytes_per_second=11.3841Gi/s
copy/large/16_median                           10.5 ns         10.5 ns            3 bytes_per_second=11.3605Gi/s
copy/large/16_stddev                          0.074 ns        0.073 ns            3 bytes_per_second=81.7966Mi/s
copy/large/16_cv                               0.71 %          0.70 %             3 bytes_per_second=0.70%
copy/large/64                                  21.0 ns         21.0 ns     13314946 bytes_per_second=22.7134Gi/s
copy/large/64                                  20.9 ns         20.9 ns     13314946 bytes_per_second=22.8413Gi/s
copy/large/64                                  21.3 ns         21.2 ns     13314946 bytes_per_second=22.452Gi/s
copy/large/64_mean                             21.1 ns         21.0 ns            3 bytes_per_second=22.6689Gi/s
copy/large/64_median                           21.0 ns         21.0 ns            3 bytes_per_second=22.7134Gi/s
copy/large/64_stddev                          0.185 ns        0.185 ns            3 bytes_per_second=203.207Mi/s
copy/large/64_cv                               0.88 %          0.88 %             3 bytes_per_second=0.88%
copy/large/256                                 29.8 ns         29.8 ns      9389671 bytes_per_second=63.9856Gi/s
copy/large/256                                 29.6 ns         29.5 ns      9389671 bytes_per_second=64.5518Gi/s
copy/large/256                                 30.2 ns         30.2 ns      9389671 bytes_per_second=63.2391Gi/s
copy/large/256_mean                            29.9 ns         29.8 ns            3 bytes_per_second=63.9255Gi/s
copy/large/256_median                          29.8 ns         29.8 ns            3 bytes_per_second=63.9856Gi/s
copy/large/256_stddev                         0.321 ns        0.308 ns            3 bytes_per_second=674.205Mi/s
copy/large/256_cv                              1.07 %          1.03 %             3 bytes_per_second=1.03%
copy/move_large/4                              1.31 ns         1.31 ns    215505630 moves_per_iter=2
copy/move_large/4                              1.29 ns         1.29 ns    215505630 moves_per_iter=2
copy/move_large/4                              1.28 ns         1.28 ns    215505630 moves_per_iter=2
copy/move_large/4_mean                         1.29 ns         1.29 ns            3 moves_per_iter=2
copy/move_large/4_median                       1.29 ns         1.29 ns            3 moves_per_iter=2
copy/move_large/4_stddev                      0.013 ns        0.013 ns            3 moves_per_iter=0
copy/move_large/4_cv                           1.00 %          1.00 %             3 moves_per_iter=0.00%
copy/move_large/16                             1.27 ns         1.27 ns    221796233 moves_per_iter=2
copy/move_large/16                             1.26 ns         1.26 ns    221796233 moves_per_iter=2
copy/move_large/16                             1.26 ns         1.26 ns    221796233 moves_per_iter=2
copy/move_large/16_mean                        1.26 ns         1.26 ns            3 moves_per_iter=2
copy/move_large/16_median                      1.26 ns         1.26 ns            3 moves_per_iter=2
copy/move_large/16_stddev                     0.006 ns        0.006 ns            3 moves_per_iter=0
copy/move_large/16_cv                          0.50 %          0.50 %             3 moves_per_iter=0.00%
copy/move_large/64                             1.26 ns         1.26 ns    222999180 moves_per_iter=2
copy/move_large/64                             1.31 ns         1.31 ns    222999180 moves_per_iter=2
copy/move_large/64                             1.26 ns         1.26 ns    222999180 moves_per_iter=2
copy/move_large/64_mean                        1.28 ns         1.28 ns            3 moves_per_iter=2
copy/move_large/64_median                      1.26 ns         1.26 ns            3 moves_per_iter=2
copy/move_large/64_stddev                     0.031 ns        0.031 ns            3 moves_per_iter=0
copy/move_large/64_cv                          2.45 %          2.44 %             3 moves_per_iter=0.00%
copy/move_large/256                            1.25 ns         1.25 ns    224618149 moves_per_iter=2
copy/move_large/256                            1.25 ns         1.25 ns    224618149 moves_per_iter=2
copy/move_large/256                            1.25 ns         1.25 ns    224618149 moves_per_iter=2
copy/move_large/256_mean                       1.25 ns         1.25 ns            3 moves_per_iter=2
copy/move_large/256_median                     1.25 ns         1.25 ns            3 moves_per_iter=2
copy/move_large/256_stddev                    0.003 ns        0.003 ns            3 moves_per_iter=0
copy/move_large/256_cv                         0.22 %          0.22 %             3 moves_per_iter=0.00%
copy/move_medium                               2.23 ns         2.23 ns    131049331
copy/move_medium                               2.26 ns         2.26 ns    131049331
copy/move_medium                               2.25 ns         2.25 ns    131049331
copy/move_medium_mean                          2.25 ns         2.25 ns            3
copy/move_medium_median                        2.25 ns         2.25 ns            3
copy/move_medium_stddev                       0.015 ns        0.015 ns            3
copy/move_medium_cv                            0.65 %          0.65 %             3
chain/small_add_10                             31.7 ns         31.6 ns      8883812 ops_per_iter=10
chain/small_add_10                             32.0 ns         32.0 ns      8883812 ops_per_iter=10
chain/small_add_10                             31.9 ns         31.9 ns      8883812 ops_per_iter=10
chain/small_add_10_mean                        31.9 ns         31.8 ns            3 ops_per_iter=10
chain/small_add_10_median                      31.9 ns         31.9 ns            3 ops_per_iter=10
chain/small_add_10_stddev                     0.187 ns        0.188 ns            3 ops_per_iter=0
chain/small_add_10_cv                          0.59 %          0.59 %             3 ops_per_iter=0.00%
chain/factorial/10                             21.0 ns         20.9 ns      9653175 ops_per_iter=9
chain/factorial/10                             23.0 ns         23.0 ns      9653175 ops_per_iter=9
chain/factorial/10                             25.0 ns         24.9 ns      9653175 ops_per_iter=9
chain/factorial/10_mean                        23.0 ns         23.0 ns            3 ops_per_iter=9
chain/factorial/10_median                      23.0 ns         23.0 ns            3 ops_per_iter=9
chain/factorial/10_stddev                      2.00 ns         2.00 ns            3 ops_per_iter=0
chain/factorial/10_cv                          8.70 %          8.71 %             3 ops_per_iter=0.00%
chain/factorial/20                             70.5 ns         70.4 ns      3904834 ops_per_iter=19
chain/factorial/20                             71.1 ns         71.0 ns      3904834 ops_per_iter=19
chain/factorial/20                             69.9 ns         69.8 ns      3904834 ops_per_iter=19
chain/factorial/20_mean                        70.5 ns         70.4 ns            3 ops_per_iter=19
chain/factorial/20_median                      70.5 ns         70.4 ns            3 ops_per_iter=19
chain/factorial/20_stddev                     0.597 ns        0.598 ns            3 ops_per_iter=0
chain/factorial/20_cv                          0.85 %          0.85 %             3 ops_per_iter=0.00%
chain/factorial/30                              111 ns          111 ns      2488557 ops_per_iter=29
chain/factorial/30                              113 ns          113 ns      2488557 ops_per_iter=29
chain/factorial/30                              114 ns          113 ns      2488557 ops_per_iter=29
chain/factorial/30_mean                         112 ns          112 ns            3 ops_per_iter=29
chain/factorial/30_median                       113 ns          113 ns            3 ops_per_iter=29
chain/factorial/30_stddev                      1.15 ns         1.15 ns            3 ops_per_iter=0
chain/factorial/30_cv                          1.02 %          1.02 %             3 ops_per_iter=0.00%
chain/factorial/50                              403 ns          402 ns       699353 ops_per_iter=49
chain/factorial/50                              401 ns          401 ns       699353 ops_per_iter=49
chain/factorial/50                              402 ns          402 ns       699353 ops_per_iter=49
chain/factorial/50_mean                         402 ns          402 ns            3 ops_per_iter=49
chain/factorial/50_median                       402 ns          402 ns            3 ops_per_iter=49
chain/factorial/50_stddev                     0.656 ns        0.649 ns            3 ops_per_iter=0
chain/factorial/50_cv                          0.16 %          0.16 %             3 ops_per_iter=0.00%
boost/small_add                                6.44 ns         6.44 ns     43003486
boost/small_add                                6.53 ns         6.52 ns     43003486
boost/small_add                                6.52 ns         6.52 ns     43003486
boost/small_add_mean                           6.50 ns         6.49 ns            3
boost/small_add_median                         6.52 ns         6.52 ns            3
boost/small_add_stddev                        0.047 ns        0.047 ns            3
boost/small_add_cv                             0.73 %          0.73 %             3
boost/small_mul                                7.98 ns         7.97 ns     34737730
boost/small_mul                                7.86 ns         7.86 ns     34737730
boost/small_mul                                7.80 ns         7.79 ns     34737730
boost/small_mul_mean                           7.88 ns         7.87 ns            3
boost/small_mul_median                         7.86 ns         7.86 ns            3
boost/small_mul_stddev                        0.091 ns        0.091 ns            3
boost/small_mul_cv                             1.16 %          1.16 %             3
boost/widening_mul                             9.39 ns         9.38 ns     30005251
boost/widening_mul                             9.30 ns         9.30 ns     30005251
boost/widening_mul                             9.32 ns         9.32 ns     30005251
boost/widening_mul_mean                        9.34 ns         9.33 ns            3
boost/widening_mul_median                      9.32 ns         9.32 ns            3
boost/widening_mul_stddev                     0.045 ns        0.044 ns            3
boost/widening_mul_cv                          0.48 %          0.47 %             3
boost/large_add/128                            9.11 ns         9.10 ns     30210828
boost/large_add/128                            9.14 ns         9.13 ns     30210828
boost/large_add/128                            9.15 ns         9.15 ns     30210828
boost/large_add/128_mean                       9.13 ns         9.13 ns            3
boost/large_add/128_median                     9.14 ns         9.13 ns            3
boost/large_add/128_stddev                    0.023 ns        0.024 ns            3
boost/large_add/128_cv                         0.26 %          0.26 %             3
boost/large_add/256                            20.9 ns         20.9 ns     13193855
boost/large_add/256                            20.9 ns         20.9 ns     13193855
boost/large_add/256                            20.8 ns         20.7 ns     13193855
boost/large_add/256_mean                       20.9 ns         20.8 ns            3
boost/large_add/256_median                     20.9 ns         20.9 ns            3
boost/large_add/256_stddev                    0.095 ns        0.094 ns            3
boost/large_add/256_cv                         0.45 %          0.45 %             3
boost/large_add/512                            21.7 ns         21.6 ns     13203810
boost/large_add/512                            21.5 ns         21.5 ns     13203810
boost/large_add/512                            21.5 ns         21.5 ns     13203810
boost/large_add/512_mean                       21.6 ns         21.5 ns            3
boost/large_add/512_median                     21.5 ns         21.5 ns            3
boost/large_add/512_stddev                    0.092 ns        0.093 ns            3
boost/large_add/512_cv                         0.43 %          0.43 %             3
boost/large_mul/128                            25.8 ns         25.8 ns     10554090
boost/large_mul/128                            27.1 ns         27.0 ns     10554090
boost/large_mul/128                            25.8 ns         25.8 ns     10554090
boost/large_mul/128_mean                       26.2 ns         26.2 ns            3
boost/large_mul/128_median                     25.8 ns         25.8 ns            3
boost/large_mul/128_stddev                    0.784 ns        0.742 ns            3
boost/large_mul/128_cv                         2.99 %          2.83 %             3
boost/large_mul/256                            36.1 ns         36.1 ns      7656340
boost/large_mul/256                            36.2 ns         36.2 ns      7656340
boost/large_mul/256                            36.4 ns         36.4 ns      7656340
boost/large_mul/256_mean                       36.2 ns         36.2 ns            3
boost/large_mul/256_median                     36.2 ns         36.2 ns            3
boost/large_mul/256_stddev                    0.142 ns        0.142 ns            3
boost/large_mul/256_cv                         0.39 %          0.39 %             3
boost/large_mul/512                            40.7 ns         40.7 ns      6851494
boost/large_mul/512                            40.8 ns         40.8 ns      6851494
boost/large_mul/512                            41.4 ns         41.4 ns      6851494
boost/large_mul/512_mean                       41.0 ns         41.0 ns            3
boost/large_mul/512_median                     40.8 ns         40.8 ns            3
boost/large_mul/512_stddev                    0.351 ns        0.353 ns            3
boost/large_mul/512_cv                         0.86 %          0.86 %             3
hydra/large_add_cmp/128                        18.0 ns         18.0 ns     15696211
hydra/large_add_cmp/128                        17.9 ns         17.9 ns     15696211
hydra/large_add_cmp/128                        18.2 ns         18.2 ns     15696211
hydra/large_add_cmp/128_mean                   18.0 ns         18.0 ns            3
hydra/large_add_cmp/128_median                 18.0 ns         18.0 ns            3
hydra/large_add_cmp/128_stddev                0.141 ns        0.142 ns            3
hydra/large_add_cmp/128_cv                     0.78 %          0.78 %             3
hydra/large_add_cmp/256                        2769 ns         2767 ns       485740
hydra/large_add_cmp/256                        2772 ns         2770 ns       485740
hydra/large_add_cmp/256                        2771 ns         2769 ns       485740
hydra/large_add_cmp/256_mean                   2771 ns         2769 ns            3
hydra/large_add_cmp/256_median                 2771 ns         2769 ns            3
hydra/large_add_cmp/256_stddev                 1.19 ns         1.21 ns            3
hydra/large_add_cmp/256_cv                     0.04 %          0.04 %             3
hydra/large_add_cmp/512                        2764 ns         2762 ns       484287
hydra/large_add_cmp/512                        2772 ns         2770 ns       484287
hydra/large_add_cmp/512                        2765 ns         2763 ns       484287
hydra/large_add_cmp/512_mean                   2767 ns         2765 ns            3
hydra/large_add_cmp/512_median                 2765 ns         2763 ns            3
hydra/large_add_cmp/512_stddev                 4.48 ns         4.51 ns            3
hydra/large_add_cmp/512_cv                     0.16 %          0.16 %             3
hydra/large_mul_cmp/128                        30.8 ns         30.7 ns      9176116
hydra/large_mul_cmp/128                        30.3 ns         30.2 ns      9176116
hydra/large_mul_cmp/128                        31.0 ns         30.9 ns      9176116
hydra/large_mul_cmp/128_mean                   30.7 ns         30.6 ns            3
hydra/large_mul_cmp/128_median                 30.8 ns         30.7 ns            3
hydra/large_mul_cmp/128_stddev                0.356 ns        0.356 ns            3
hydra/large_mul_cmp/128_cv                     1.16 %          1.16 %             3
hydra/large_mul_cmp/256                        30.5 ns         30.5 ns      9025562
hydra/large_mul_cmp/256                        30.4 ns         30.3 ns      9025562
hydra/large_mul_cmp/256                        30.7 ns         30.7 ns      9025562
hydra/large_mul_cmp/256_mean                   30.5 ns         30.5 ns            3
hydra/large_mul_cmp/256_median                 30.5 ns         30.5 ns            3
hydra/large_mul_cmp/256_stddev                0.191 ns        0.191 ns            3
hydra/large_mul_cmp/256_cv                     0.63 %          0.63 %             3
hydra/large_mul_cmp/512                        66.8 ns         66.8 ns      4128393
hydra/large_mul_cmp/512                        67.5 ns         67.5 ns      4128393
hydra/large_mul_cmp/512                        67.5 ns         67.4 ns      4128393
hydra/large_mul_cmp/512_mean                   67.3 ns         67.2 ns            3
hydra/large_mul_cmp/512_median                 67.5 ns         67.4 ns            3
hydra/large_mul_cmp/512_stddev                0.391 ns        0.393 ns            3
hydra/large_mul_cmp/512_cv                     0.58 %          0.59 %             3

── Comparison report ───────────────────────────────────────────────────
# Hydra Benchmark Report

Generated: `2026-04-15 08:00`  Machine: `Henrys-MacBook-Pro.local`  CPUs: `18 × 24 MHz`  Build: `release`

> Source: `bench_results.json`  Metric: `cpu_time`

### Small operations vs. native uint64_t
*Lower Δ is better. Goal: < 2× native for small ops. · metric: `cpu_time`*

| Operation | Subject (ns) | Reference (ns) | Ratio | Δ |
|-----------|-------------|----------------|-------|---|
| small add | 3.11 ns | 2.47 ns `baseline/u64_add` | 1.26× | +26.1% |
| small mul | 4.03 ns | 3.40 ns `baseline/u64_mul` | 1.19× | +18.5% |
| small sub (vs add) | 1.09 ns | 2.47 ns `baseline/u64_add` | 0.44× | -55.6% |
| widening add (vs native) | 3.18 ns | 2.47 ns `baseline/u64_add` | 1.29× | +28.8% |
| widening mul 128 (vs native) | 3.79 ns | 3.40 ns `baseline/u64_mul` | 1.12× | +11.7% |

### Medium / Large operations vs. Boost.Multiprecision
*Negative Δ means Hydra is faster than Boost. · metric: `cpu_time`*

| Operation | Subject (ns) | Reference (ns) | Ratio | Δ |
|-----------|-------------|----------------|-------|---|
| widening mul 128 | 3.79 ns | 9.32 ns `boost/widening_mul` | 0.41× | -59.3% |
| medium add (vs large/128) | 6.34 ns | 9.15 ns `boost/large_add/128` | 0.69× | -30.6% |
| medium mul (vs large/128) | 18.22 ns | 25.76 ns `boost/large_mul/128` | 0.71× | -29.3% |
| small add | 3.11 ns | 6.52 ns `boost/small_add` | 0.48× | -52.3% |
| small mul | 4.03 ns | 7.79 ns `boost/small_mul` | 0.52× | -48.3% |
| large add 128-bit | 18.19 ns | 9.15 ns `boost/large_add/128` | 1.99× | +98.9% |
| large add 256-bit | 2.77 µs | 20.74 ns `boost/large_add/256` | 133.53× | +13252.7% ⚠ |
| large add 512-bit | 2.76 µs | 21.45 ns `boost/large_add/512` | 128.79× | +12779.4% ⚠ |
| large mul 128-bit | 30.94 ns | 25.76 ns `boost/large_mul/128` | 1.20× | +20.1% |
| large mul 256-bit | 30.70 ns | 36.36 ns `boost/large_mul/256` | 0.84× | -15.6% |
| large mul 512-bit | 67.41 ns | 41.37 ns `boost/large_mul/512` | 1.63× | +63.0% |

  (skipped 1 pair(s) — benchmarks not in results)

### Allocation costs

| Benchmark | 4 limbs | 8 limbs | 16 limbs | 64 limbs | 256 limbs |
|-----------|------------|------------|------------|------------|------------|
| `alloc/from_limbs` | 12.02 ns | 9.96 ns | 10.24 ns | 20.59 ns | 28.86 ns |
| `alloc/largerep_clone` | 12.13 ns | — | 10.04 ns | 19.29 ns | 27.76 ns |
| `alloc/largerep_create_destroy` | 10.02 ns | — | 8.56 ns | 15.94 ns | 13.16 ns |

| Benchmark | Time |
|-----------|------|
| `alloc/normalize_large_to_medium` | 14.19 ns |
| `alloc/normalize_medium_to_small` | 2.01 ns |

### Copy / Move costs

| Benchmark | 4 limbs | 16 limbs | 64 limbs | 256 limbs |
|-----------|------------|------------|------------|------------|
| `copy/large` | 12.42 ns | 10.53 ns | 21.24 ns | 30.16 ns |
| `copy/move_large` | 1.28 ns | 1.26 ns | 1.26 ns | 1.25 ns |

| Benchmark | Time |
|-----------|------|
| `copy/medium` | 435.5 ps |
| `copy/move_medium` | 2.25 ns |
| `copy/small` | 335.7 ps |

### Arithmetic chain throughput

| Benchmark | 10 limbs | 20 limbs | 30 limbs | 50 limbs |
|-----------|------------|------------|------------|------------|
| `chain/factorial` | 24.94 ns | 69.81 ns | 113.50 ns | 401.95 ns |

| Benchmark | Time |
|-----------|------|
| `chain/small_add_10` | 31.86 ns |

---

**Note:** Some comparison pairs were skipped.
Run with `--benchmark_filter=all` or enable Boost benchmarks.

