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
baseline/u64_add                               2.66 ns         2.66 ns    118966689
baseline/u64_add                               2.66 ns         2.66 ns    118966689
baseline/u64_add                               2.64 ns         2.64 ns    118966689
baseline/u64_add_mean                          2.65 ns         2.65 ns            3
baseline/u64_add_median                        2.66 ns         2.66 ns            3
baseline/u64_add_stddev                       0.011 ns        0.011 ns            3
baseline/u64_add_cv                            0.41 %          0.40 %             3
baseline/u64_mul                               3.52 ns         3.52 ns     83802227
baseline/u64_mul                               3.52 ns         3.52 ns     83802227
baseline/u64_mul                               3.53 ns         3.53 ns     83802227
baseline/u64_mul_mean                          3.52 ns         3.52 ns            3
baseline/u64_mul_median                        3.52 ns         3.52 ns            3
baseline/u64_mul_stddev                       0.005 ns        0.005 ns            3
baseline/u64_mul_cv                            0.14 %          0.13 %             3
hydra/small_add                                3.04 ns         3.04 ns     84365302
hydra/small_add                                2.97 ns         2.97 ns     84365302
hydra/small_add                                2.98 ns         2.97 ns     84365302
hydra/small_add_mean                           3.00 ns         3.00 ns            3
hydra/small_add_median                         2.98 ns         2.97 ns            3
hydra/small_add_stddev                        0.040 ns        0.040 ns            3
hydra/small_add_cv                             1.35 %          1.35 %             3
hydra/small_mul                                4.00 ns         3.99 ns     70147309
hydra/small_mul                                4.00 ns         3.99 ns     70147309
hydra/small_mul                                4.00 ns         4.00 ns     70147309
hydra/small_mul_mean                           4.00 ns         3.99 ns            3
hydra/small_mul_median                         4.00 ns         3.99 ns            3
hydra/small_mul_stddev                        0.003 ns        0.003 ns            3
hydra/small_mul_cv                             0.08 %          0.08 %             3
hydra/small_sub                                1.08 ns         1.08 ns    252072849
hydra/small_sub                                1.07 ns         1.07 ns    252072849
hydra/small_sub                                1.06 ns         1.06 ns    252072849
hydra/small_sub_mean                           1.07 ns         1.07 ns            3
hydra/small_sub_median                         1.07 ns         1.07 ns            3
hydra/small_sub_stddev                        0.007 ns        0.007 ns            3
hydra/small_sub_cv                             0.63 %          0.64 %             3
hydra/widening_add                             3.16 ns         3.15 ns     88610399
hydra/widening_add                             3.16 ns         3.16 ns     88610399
hydra/widening_add                             3.16 ns         3.16 ns     88610399
hydra/widening_add_mean                        3.16 ns         3.16 ns            3
hydra/widening_add_median                      3.16 ns         3.16 ns            3
hydra/widening_add_stddev                     0.001 ns        0.001 ns            3
hydra/widening_add_cv                          0.05 %          0.05 %             3
hydra/widening_mul_128                         3.78 ns         3.78 ns     74168256
hydra/widening_mul_128                         3.78 ns         3.78 ns     74168256
hydra/widening_mul_128                         3.78 ns         3.78 ns     74168256
hydra/widening_mul_128_mean                    3.78 ns         3.78 ns            3
hydra/widening_mul_128_median                  3.78 ns         3.78 ns            3
hydra/widening_mul_128_stddev                 0.001 ns        0.001 ns            3
hydra/widening_mul_128_cv                      0.03 %          0.03 %             3
hydra/medium_add                               6.40 ns         6.40 ns     42511843
hydra/medium_add                               6.41 ns         6.41 ns     42511843
hydra/medium_add                               6.41 ns         6.41 ns     42511843
hydra/medium_add_mean                          6.41 ns         6.41 ns            3
hydra/medium_add_median                        6.41 ns         6.41 ns            3
hydra/medium_add_stddev                       0.006 ns        0.006 ns            3
hydra/medium_add_cv                            0.09 %          0.09 %             3
hydra/medium_mul                               18.6 ns         18.5 ns     14990738
hydra/medium_mul                               18.5 ns         18.5 ns     14990738
hydra/medium_mul                               18.5 ns         18.5 ns     14990738
hydra/medium_mul_mean                          18.5 ns         18.5 ns            3
hydra/medium_mul_median                        18.5 ns         18.5 ns            3
hydra/medium_mul_stddev                       0.028 ns        0.028 ns            3
hydra/medium_mul_cv                            0.15 %          0.15 %             3
alloc/largerep_create_destroy/4                9.76 ns         9.75 ns     28819927 items_per_second=102.58M/s
alloc/largerep_create_destroy/4                9.77 ns         9.77 ns     28819927 items_per_second=102.405M/s
alloc/largerep_create_destroy/4                9.80 ns         9.80 ns     28819927 items_per_second=102.064M/s
alloc/largerep_create_destroy/4_mean           9.78 ns         9.77 ns            3 items_per_second=102.35M/s
alloc/largerep_create_destroy/4_median         9.77 ns         9.77 ns            3 items_per_second=102.405M/s
alloc/largerep_create_destroy/4_stddev        0.025 ns        0.025 ns            3 items_per_second=262.34k/s
alloc/largerep_create_destroy/4_cv             0.26 %          0.26 %             3 items_per_second=0.26%
alloc/largerep_create_destroy/16               8.49 ns         8.48 ns     32730928 items_per_second=117.877M/s
alloc/largerep_create_destroy/16               8.49 ns         8.48 ns     32730928 items_per_second=117.886M/s
alloc/largerep_create_destroy/16               8.48 ns         8.48 ns     32730928 items_per_second=117.967M/s
alloc/largerep_create_destroy/16_mean          8.49 ns         8.48 ns            3 items_per_second=117.91M/s
alloc/largerep_create_destroy/16_median        8.49 ns         8.48 ns            3 items_per_second=117.886M/s
alloc/largerep_create_destroy/16_stddev       0.003 ns        0.004 ns            3 items_per_second=49.4863k/s
alloc/largerep_create_destroy/16_cv            0.04 %          0.04 %             3 items_per_second=0.04%
alloc/largerep_create_destroy/64               15.7 ns         15.7 ns     17593576 items_per_second=63.5393M/s
alloc/largerep_create_destroy/64               15.8 ns         15.8 ns     17593576 items_per_second=63.3946M/s
alloc/largerep_create_destroy/64               15.7 ns         15.7 ns     17593576 items_per_second=63.651M/s
alloc/largerep_create_destroy/64_mean          15.8 ns         15.7 ns            3 items_per_second=63.5283M/s
alloc/largerep_create_destroy/64_median        15.7 ns         15.7 ns            3 items_per_second=63.5393M/s
alloc/largerep_create_destroy/64_stddev       0.032 ns        0.032 ns            3 items_per_second=128.561k/s
alloc/largerep_create_destroy/64_cv            0.20 %          0.20 %             3 items_per_second=0.20%
alloc/largerep_create_destroy/256              13.2 ns         13.2 ns     21268353 items_per_second=76.0358M/s
alloc/largerep_create_destroy/256              13.2 ns         13.1 ns     21268353 items_per_second=76.0679M/s
alloc/largerep_create_destroy/256              13.2 ns         13.1 ns     21268353 items_per_second=76.0782M/s
alloc/largerep_create_destroy/256_mean         13.2 ns         13.1 ns            3 items_per_second=76.0606M/s
alloc/largerep_create_destroy/256_median       13.2 ns         13.1 ns            3 items_per_second=76.0679M/s
alloc/largerep_create_destroy/256_stddev      0.004 ns        0.004 ns            3 items_per_second=22.1244k/s
alloc/largerep_create_destroy/256_cv           0.03 %          0.03 %             3 items_per_second=0.03%
alloc/from_limbs/4                             12.1 ns         12.1 ns     22869651 items_per_second=82.7274M/s
alloc/from_limbs/4                             12.2 ns         12.2 ns     22869651 items_per_second=81.7708M/s
alloc/from_limbs/4                             12.1 ns         12.1 ns     22869651 items_per_second=82.7477M/s
alloc/from_limbs/4_mean                        12.1 ns         12.1 ns            3 items_per_second=82.4153M/s
alloc/from_limbs/4_median                      12.1 ns         12.1 ns            3 items_per_second=82.7274M/s
alloc/from_limbs/4_stddev                     0.083 ns        0.083 ns            3 items_per_second=558.259k/s
alloc/from_limbs/4_cv                          0.68 %          0.68 %             3 items_per_second=0.68%
alloc/from_limbs/8                             9.86 ns         9.85 ns     28516723 items_per_second=101.539M/s
alloc/from_limbs/8                             9.86 ns         9.85 ns     28516723 items_per_second=101.546M/s
alloc/from_limbs/8                             9.82 ns         9.81 ns     28516723 items_per_second=101.947M/s
alloc/from_limbs/8_mean                        9.84 ns         9.84 ns            3 items_per_second=101.677M/s
alloc/from_limbs/8_median                      9.86 ns         9.85 ns            3 items_per_second=101.546M/s
alloc/from_limbs/8_stddev                     0.023 ns        0.023 ns            3 items_per_second=233.607k/s
alloc/from_limbs/8_cv                          0.23 %          0.23 %             3 items_per_second=0.23%
alloc/from_limbs/16                            10.1 ns         10.1 ns     27036683 items_per_second=99.0366M/s
alloc/from_limbs/16                            10.0 ns         10.0 ns     27036683 items_per_second=99.7038M/s
alloc/from_limbs/16                            10.1 ns         10.0 ns     27036683 items_per_second=99.5595M/s
alloc/from_limbs/16_mean                       10.1 ns         10.1 ns            3 items_per_second=99.4333M/s
alloc/from_limbs/16_median                     10.1 ns         10.0 ns            3 items_per_second=99.5595M/s
alloc/from_limbs/16_stddev                    0.036 ns        0.036 ns            3 items_per_second=351.08k/s
alloc/from_limbs/16_cv                         0.35 %          0.35 %             3 items_per_second=0.35%
alloc/from_limbs/64                            20.4 ns         20.4 ns     13622653 items_per_second=48.9898M/s
alloc/from_limbs/64                            20.4 ns         20.4 ns     13622653 items_per_second=49.0781M/s
alloc/from_limbs/64                            20.4 ns         20.4 ns     13622653 items_per_second=49.0126M/s
alloc/from_limbs/64_mean                       20.4 ns         20.4 ns            3 items_per_second=49.0268M/s
alloc/from_limbs/64_median                     20.4 ns         20.4 ns            3 items_per_second=49.0126M/s
alloc/from_limbs/64_stddev                    0.019 ns        0.019 ns            3 items_per_second=45.8188k/s
alloc/from_limbs/64_cv                         0.09 %          0.09 %             3 items_per_second=0.09%
alloc/from_limbs/256                           28.9 ns         28.9 ns      9664837 items_per_second=34.6374M/s
alloc/from_limbs/256                           28.8 ns         28.8 ns      9664837 items_per_second=34.768M/s
alloc/from_limbs/256                           28.7 ns         28.7 ns      9664837 items_per_second=34.9027M/s
alloc/from_limbs/256_mean                      28.8 ns         28.8 ns            3 items_per_second=34.7694M/s
alloc/from_limbs/256_median                    28.8 ns         28.8 ns            3 items_per_second=34.768M/s
alloc/from_limbs/256_stddev                   0.109 ns        0.110 ns            3 items_per_second=132.659k/s
alloc/from_limbs/256_cv                        0.38 %          0.38 %             3 items_per_second=0.38%
alloc/largerep_clone/4                         11.7 ns         11.7 ns     23754380 items_per_second=85.5693M/s
alloc/largerep_clone/4                         11.8 ns         11.8 ns     23754380 items_per_second=84.4564M/s
alloc/largerep_clone/4                         12.0 ns         12.0 ns     23754380 items_per_second=83.5507M/s
alloc/largerep_clone/4_mean                    11.8 ns         11.8 ns            3 items_per_second=84.5255M/s
alloc/largerep_clone/4_median                  11.8 ns         11.8 ns            3 items_per_second=84.4564M/s
alloc/largerep_clone/4_stddev                 0.141 ns        0.141 ns            3 items_per_second=1.01108M/s
alloc/largerep_clone/4_cv                      1.19 %          1.19 %             3 items_per_second=1.20%
alloc/largerep_clone/16                        9.81 ns         9.80 ns     28675891 items_per_second=102.002M/s
alloc/largerep_clone/16                        9.80 ns         9.79 ns     28675891 items_per_second=102.16M/s
alloc/largerep_clone/16                        9.78 ns         9.77 ns     28675891 items_per_second=102.332M/s
alloc/largerep_clone/16_mean                   9.80 ns         9.79 ns            3 items_per_second=102.165M/s
alloc/largerep_clone/16_median                 9.80 ns         9.79 ns            3 items_per_second=102.16M/s
alloc/largerep_clone/16_stddev                0.016 ns        0.016 ns            3 items_per_second=164.934k/s
alloc/largerep_clone/16_cv                     0.16 %          0.16 %             3 items_per_second=0.16%
alloc/largerep_clone/64                        20.1 ns         20.1 ns     13886134 items_per_second=49.6941M/s
alloc/largerep_clone/64                        20.3 ns         20.2 ns     13886134 items_per_second=49.3889M/s
alloc/largerep_clone/64                        20.2 ns         20.2 ns     13886134 items_per_second=49.4367M/s
alloc/largerep_clone/64_mean                   20.2 ns         20.2 ns            3 items_per_second=49.5066M/s
alloc/largerep_clone/64_median                 20.2 ns         20.2 ns            3 items_per_second=49.4367M/s
alloc/largerep_clone/64_stddev                0.068 ns        0.067 ns            3 items_per_second=164.177k/s
alloc/largerep_clone/64_cv                     0.33 %          0.33 %             3 items_per_second=0.33%
alloc/largerep_clone/256                       28.6 ns         28.5 ns      9983598 items_per_second=35.045M/s
alloc/largerep_clone/256                       28.6 ns         28.5 ns      9983598 items_per_second=35.0366M/s
alloc/largerep_clone/256                       28.4 ns         28.4 ns      9983598 items_per_second=35.1973M/s
alloc/largerep_clone/256_mean                  28.5 ns         28.5 ns            3 items_per_second=35.093M/s
alloc/largerep_clone/256_median                28.6 ns         28.5 ns            3 items_per_second=35.045M/s
alloc/largerep_clone/256_stddev               0.074 ns        0.073 ns            3 items_per_second=90.4311k/s
alloc/largerep_clone/256_cv                    0.26 %          0.26 %             3 items_per_second=0.26%
alloc/normalize_large_to_medium                13.8 ns         13.8 ns     20229752
alloc/normalize_large_to_medium                13.8 ns         13.8 ns     20229752
alloc/normalize_large_to_medium                13.8 ns         13.8 ns     20229752
alloc/normalize_large_to_medium_mean           13.8 ns         13.8 ns            3
alloc/normalize_large_to_medium_median         13.8 ns         13.8 ns            3
alloc/normalize_large_to_medium_stddev        0.010 ns        0.010 ns            3
alloc/normalize_large_to_medium_cv             0.07 %          0.07 %             3
alloc/normalize_medium_to_small                1.54 ns         1.54 ns    174035192
alloc/normalize_medium_to_small                1.55 ns         1.55 ns    174035192
alloc/normalize_medium_to_small                1.55 ns         1.55 ns    174035192
alloc/normalize_medium_to_small_mean           1.55 ns         1.55 ns            3
alloc/normalize_medium_to_small_median         1.55 ns         1.55 ns            3
alloc/normalize_medium_to_small_stddev        0.006 ns        0.005 ns            3
alloc/normalize_medium_to_small_cv             0.36 %          0.36 %             3
copy/small                                    0.331 ns        0.331 ns    748943455
copy/small                                    0.324 ns        0.324 ns    748943455
copy/small                                    0.325 ns        0.325 ns    748943455
copy/small_mean                               0.327 ns        0.326 ns            3
copy/small_median                             0.325 ns        0.325 ns            3
copy/small_stddev                             0.004 ns        0.004 ns            3
copy/small_cv                                  1.12 %          1.12 %             3
copy/medium                                   0.439 ns        0.439 ns    615993840
copy/medium                                   0.443 ns        0.442 ns    615993840
copy/medium                                   0.444 ns        0.444 ns    615993840
copy/medium_mean                              0.442 ns        0.442 ns            3
copy/medium_median                            0.443 ns        0.442 ns            3
copy/medium_stddev                            0.003 ns        0.003 ns            3
copy/medium_cv                                 0.63 %          0.62 %             3
copy/large/4                                   12.5 ns         12.5 ns     22542105 bytes_per_second=2.38609Gi/s
copy/large/4                                   12.6 ns         12.6 ns     22542105 bytes_per_second=2.36504Gi/s
copy/large/4                                   12.5 ns         12.5 ns     22542105 bytes_per_second=2.38006Gi/s
copy/large/4_mean                              12.5 ns         12.5 ns            3 bytes_per_second=2.37706Gi/s
copy/large/4_median                            12.5 ns         12.5 ns            3 bytes_per_second=2.38006Gi/s
copy/large/4_stddev                           0.057 ns        0.057 ns            3 bytes_per_second=11.0963Mi/s
copy/large/4_cv                                0.45 %          0.46 %             3 bytes_per_second=0.46%
copy/large/16                                  10.3 ns         10.3 ns     26555135 bytes_per_second=11.5618Gi/s
copy/large/16                                  10.3 ns         10.3 ns     26555135 bytes_per_second=11.5385Gi/s
copy/large/16                                  10.3 ns         10.3 ns     26555135 bytes_per_second=11.5478Gi/s
copy/large/16_mean                             10.3 ns         10.3 ns            3 bytes_per_second=11.5494Gi/s
copy/large/16_median                           10.3 ns         10.3 ns            3 bytes_per_second=11.5478Gi/s
copy/large/16_stddev                          0.010 ns        0.011 ns            3 bytes_per_second=12.0355Mi/s
copy/large/16_cv                               0.10 %          0.10 %             3 bytes_per_second=0.10%
copy/large/64                                  20.8 ns         20.8 ns     13266998 bytes_per_second=22.9355Gi/s
copy/large/64                                  20.7 ns         20.7 ns     13266998 bytes_per_second=23.0234Gi/s
copy/large/64                                  20.8 ns         20.8 ns     13266998 bytes_per_second=22.9632Gi/s
copy/large/64_mean                             20.8 ns         20.8 ns            3 bytes_per_second=22.974Gi/s
copy/large/64_median                           20.8 ns         20.8 ns            3 bytes_per_second=22.9632Gi/s
copy/large/64_stddev                          0.040 ns        0.041 ns            3 bytes_per_second=45.9764Mi/s
copy/large/64_cv                               0.19 %          0.20 %             3 bytes_per_second=0.20%
copy/large/256                                 29.0 ns         29.0 ns      9653508 bytes_per_second=65.7558Gi/s
copy/large/256                                 29.1 ns         29.0 ns      9653508 bytes_per_second=65.6613Gi/s
copy/large/256                                 29.0 ns         29.0 ns      9653508 bytes_per_second=65.851Gi/s
copy/large/256_mean                            29.0 ns         29.0 ns            3 bytes_per_second=65.756Gi/s
copy/large/256_median                          29.0 ns         29.0 ns            3 bytes_per_second=65.7558Gi/s
copy/large/256_stddev                         0.042 ns        0.042 ns            3 bytes_per_second=97.1492Mi/s
copy/large/256_cv                              0.14 %          0.14 %             3 bytes_per_second=0.14%
copy/move_large/4                              1.24 ns         1.24 ns    224894179 moves_per_iter=2
copy/move_large/4                              1.25 ns         1.25 ns    224894179 moves_per_iter=2
copy/move_large/4                              1.25 ns         1.24 ns    224894179 moves_per_iter=2
copy/move_large/4_mean                         1.25 ns         1.25 ns            3 moves_per_iter=2
copy/move_large/4_median                       1.25 ns         1.24 ns            3 moves_per_iter=2
copy/move_large/4_stddev                      0.002 ns        0.002 ns            3 moves_per_iter=0
copy/move_large/4_cv                           0.17 %          0.17 %             3 moves_per_iter=0.00%
copy/move_large/16                             1.24 ns         1.24 ns    225192620 moves_per_iter=2
copy/move_large/16                             1.24 ns         1.24 ns    225192620 moves_per_iter=2
copy/move_large/16                             1.25 ns         1.24 ns    225192620 moves_per_iter=2
copy/move_large/16_mean                        1.25 ns         1.24 ns            3 moves_per_iter=2
copy/move_large/16_median                      1.24 ns         1.24 ns            3 moves_per_iter=2
copy/move_large/16_stddev                     0.000 ns        0.000 ns            3 moves_per_iter=0
copy/move_large/16_cv                          0.02 %          0.02 %             3 moves_per_iter=0.00%
copy/move_large/64                             1.24 ns         1.24 ns    225111149 moves_per_iter=2
copy/move_large/64                             1.25 ns         1.24 ns    225111149 moves_per_iter=2
copy/move_large/64                             1.25 ns         1.24 ns    225111149 moves_per_iter=2
copy/move_large/64_mean                        1.24 ns         1.24 ns            3 moves_per_iter=2
copy/move_large/64_median                      1.25 ns         1.24 ns            3 moves_per_iter=2
copy/move_large/64_stddev                     0.000 ns        0.000 ns            3 moves_per_iter=0
copy/move_large/64_cv                          0.04 %          0.04 %             3 moves_per_iter=0.00%
copy/move_large/256                            1.24 ns         1.24 ns    225040588 moves_per_iter=2
copy/move_large/256                            1.25 ns         1.25 ns    225040588 moves_per_iter=2
copy/move_large/256                            1.24 ns         1.24 ns    225040588 moves_per_iter=2
copy/move_large/256_mean                       1.25 ns         1.24 ns            3 moves_per_iter=2
copy/move_large/256_median                     1.24 ns         1.24 ns            3 moves_per_iter=2
copy/move_large/256_stddev                    0.001 ns        0.001 ns            3 moves_per_iter=0
copy/move_large/256_cv                         0.08 %          0.08 %             3 moves_per_iter=0.00%
copy/move_medium                               2.22 ns         2.22 ns    127684801
copy/move_medium                               2.23 ns         2.23 ns    127684801
copy/move_medium                               2.23 ns         2.23 ns    127684801
copy/move_medium_mean                          2.23 ns         2.23 ns            3
copy/move_medium_median                        2.23 ns         2.23 ns            3
copy/move_medium_stddev                       0.009 ns        0.009 ns            3
copy/move_medium_cv                            0.42 %          0.42 %             3
chain/small_add_10                             31.5 ns         31.5 ns      8895101 ops_per_iter=10
chain/small_add_10                             31.5 ns         31.4 ns      8895101 ops_per_iter=10
chain/small_add_10                             31.5 ns         31.5 ns      8895101 ops_per_iter=10
chain/small_add_10_mean                        31.5 ns         31.5 ns            3 ops_per_iter=10
chain/small_add_10_median                      31.5 ns         31.5 ns            3 ops_per_iter=10
chain/small_add_10_stddev                     0.006 ns        0.006 ns            3 ops_per_iter=0
chain/small_add_10_cv                          0.02 %          0.02 %             3 ops_per_iter=0.00%
chain/factorial/10                             18.5 ns         18.5 ns     11034048 ops_per_iter=9
chain/factorial/10                             17.1 ns         17.1 ns     11034048 ops_per_iter=9
chain/factorial/10                             17.5 ns         17.5 ns     11034048 ops_per_iter=9
chain/factorial/10_mean                        17.7 ns         17.7 ns            3 ops_per_iter=9
chain/factorial/10_median                      17.5 ns         17.5 ns            3 ops_per_iter=9
chain/factorial/10_stddev                     0.724 ns        0.724 ns            3 ops_per_iter=0
chain/factorial/10_cv                          4.09 %          4.09 %             3 ops_per_iter=0.00%
chain/factorial/20                             66.9 ns         66.8 ns      4266732 ops_per_iter=19
chain/factorial/20                             67.1 ns         67.0 ns      4266732 ops_per_iter=19
chain/factorial/20                             67.1 ns         67.0 ns      4266732 ops_per_iter=19
chain/factorial/20_mean                        67.0 ns         67.0 ns            3 ops_per_iter=19
chain/factorial/20_median                      67.1 ns         67.0 ns            3 ops_per_iter=19
chain/factorial/20_stddev                     0.128 ns        0.128 ns            3 ops_per_iter=0
chain/factorial/20_cv                          0.19 %          0.19 %             3 ops_per_iter=0.00%
chain/factorial/30                              130 ns          130 ns      2159944 ops_per_iter=29
chain/factorial/30                              130 ns          130 ns      2159944 ops_per_iter=29
chain/factorial/30                              130 ns          130 ns      2159944 ops_per_iter=29
chain/factorial/30_mean                         130 ns          130 ns            3 ops_per_iter=29
chain/factorial/30_median                       130 ns          130 ns            3 ops_per_iter=29
chain/factorial/30_stddev                     0.302 ns        0.300 ns            3 ops_per_iter=0
chain/factorial/30_cv                          0.23 %          0.23 %             3 ops_per_iter=0.00%
chain/factorial/50                              403 ns          403 ns       698551 ops_per_iter=49
chain/factorial/50                              401 ns          401 ns       698551 ops_per_iter=49
chain/factorial/50                              400 ns          400 ns       698551 ops_per_iter=49
chain/factorial/50_mean                         402 ns          401 ns            3 ops_per_iter=49
chain/factorial/50_median                       401 ns          401 ns            3 ops_per_iter=49
chain/factorial/50_stddev                      1.28 ns         1.28 ns            3 ops_per_iter=0
chain/factorial/50_cv                          0.32 %          0.32 %             3 ops_per_iter=0.00%
boost/small_add                                6.39 ns         6.39 ns     43468811
boost/small_add                                6.32 ns         6.32 ns     43468811
boost/small_add                                6.32 ns         6.32 ns     43468811
boost/small_add_mean                           6.35 ns         6.34 ns            3
boost/small_add_median                         6.32 ns         6.32 ns            3
boost/small_add_stddev                        0.038 ns        0.038 ns            3
boost/small_add_cv                             0.60 %          0.60 %             3
boost/small_mul                                7.71 ns         7.71 ns     35413900
boost/small_mul                                7.73 ns         7.72 ns     35413900
boost/small_mul                                7.70 ns         7.70 ns     35413900
boost/small_mul_mean                           7.72 ns         7.71 ns            3
boost/small_mul_median                         7.71 ns         7.71 ns            3
boost/small_mul_stddev                        0.014 ns        0.014 ns            3
boost/small_mul_cv                             0.18 %          0.18 %             3
boost/widening_mul                             9.32 ns         9.32 ns     29297897
boost/widening_mul                             9.20 ns         9.19 ns     29297897
boost/widening_mul                             9.19 ns         9.18 ns     29297897
boost/widening_mul_mean                        9.24 ns         9.23 ns            3
boost/widening_mul_median                      9.20 ns         9.19 ns            3
boost/widening_mul_stddev                     0.076 ns        0.076 ns            3
boost/widening_mul_cv                          0.82 %          0.82 %             3
boost/large_add/128                            9.07 ns         9.07 ns     30521038
boost/large_add/128                            9.05 ns         9.04 ns     30521038
boost/large_add/128                            9.05 ns         9.04 ns     30521038
boost/large_add/128_mean                       9.06 ns         9.05 ns            3
boost/large_add/128_median                     9.05 ns         9.04 ns            3
boost/large_add/128_stddev                    0.015 ns        0.015 ns            3
boost/large_add/128_cv                         0.16 %          0.17 %             3
boost/large_add/256                            21.6 ns         21.6 ns     12767898
boost/large_add/256                            21.5 ns         21.5 ns     12767898
boost/large_add/256                            21.4 ns         21.4 ns     12767898
boost/large_add/256_mean                       21.5 ns         21.5 ns            3
boost/large_add/256_median                     21.5 ns         21.5 ns            3
boost/large_add/256_stddev                    0.085 ns        0.084 ns            3
boost/large_add/256_cv                         0.39 %          0.39 %             3
boost/large_add/512                            22.0 ns         22.0 ns     12862327
boost/large_add/512                            21.8 ns         21.8 ns     12862327
boost/large_add/512                            21.7 ns         21.7 ns     12862327
boost/large_add/512_mean                       21.8 ns         21.8 ns            3
boost/large_add/512_median                     21.8 ns         21.8 ns            3
boost/large_add/512_stddev                    0.177 ns        0.176 ns            3
boost/large_add/512_cv                         0.81 %          0.81 %             3
boost/large_mul/128                            25.2 ns         25.2 ns     10859869
boost/large_mul/128                            25.0 ns         25.0 ns     10859869
boost/large_mul/128                            24.7 ns         24.7 ns     10859869
boost/large_mul/128_mean                       25.0 ns         25.0 ns            3
boost/large_mul/128_median                     25.0 ns         25.0 ns            3
boost/large_mul/128_stddev                    0.241 ns        0.241 ns            3
boost/large_mul/128_cv                         0.96 %          0.96 %             3
boost/large_mul/256                            36.6 ns         36.5 ns      7539244
boost/large_mul/256                            36.4 ns         36.4 ns      7539244
boost/large_mul/256                            36.4 ns         36.4 ns      7539244
boost/large_mul/256_mean                       36.5 ns         36.4 ns            3
boost/large_mul/256_median                     36.4 ns         36.4 ns            3
boost/large_mul/256_stddev                    0.100 ns        0.100 ns            3
boost/large_mul/256_cv                         0.28 %          0.27 %             3
boost/large_mul/512                            40.9 ns         40.8 ns      6910679
boost/large_mul/512                            41.0 ns         41.0 ns      6910679
boost/large_mul/512                            40.6 ns         40.5 ns      6910679
boost/large_mul/512_mean                       40.8 ns         40.8 ns            3
boost/large_mul/512_median                     40.9 ns         40.8 ns            3
boost/large_mul/512_stddev                    0.218 ns        0.217 ns            3
boost/large_mul/512_cv                         0.53 %          0.53 %             3
hydra/large_add_cmp/128                        18.3 ns         18.3 ns     15300630
hydra/large_add_cmp/128                        18.2 ns         18.2 ns     15300630
hydra/large_add_cmp/128                        18.3 ns         18.3 ns     15300630
hydra/large_add_cmp/128_mean                   18.3 ns         18.3 ns            3
hydra/large_add_cmp/128_median                 18.3 ns         18.3 ns            3
hydra/large_add_cmp/128_stddev                0.025 ns        0.025 ns            3
hydra/large_add_cmp/128_cv                     0.13 %          0.13 %             3
hydra/large_add_cmp/256                        2650 ns         2648 ns       570823
hydra/large_add_cmp/256                        2651 ns         2649 ns       570823
hydra/large_add_cmp/256                        2652 ns         2650 ns       570823
hydra/large_add_cmp/256_mean                   2651 ns         2649 ns            3
hydra/large_add_cmp/256_median                 2651 ns         2649 ns            3
hydra/large_add_cmp/256_stddev                0.792 ns        0.800 ns            3
hydra/large_add_cmp/256_cv                     0.03 %          0.03 %             3
hydra/large_add_cmp/512                        2637 ns         2635 ns       567341
hydra/large_add_cmp/512                        2638 ns         2636 ns       567341
hydra/large_add_cmp/512                        2635 ns         2633 ns       567341
hydra/large_add_cmp/512_mean                   2637 ns         2635 ns            3
hydra/large_add_cmp/512_median                 2637 ns         2635 ns            3
hydra/large_add_cmp/512_stddev                 1.28 ns         1.28 ns            3
hydra/large_add_cmp/512_cv                     0.05 %          0.05 %             3
hydra/large_mul_cmp/128                        33.2 ns         33.2 ns      8074051
hydra/large_mul_cmp/128                        32.4 ns         32.3 ns      8074051
hydra/large_mul_cmp/128                        32.2 ns         32.2 ns      8074051
hydra/large_mul_cmp/128_mean                   32.6 ns         32.6 ns            3
hydra/large_mul_cmp/128_median                 32.4 ns         32.3 ns            3
hydra/large_mul_cmp/128_stddev                0.550 ns        0.551 ns            3
hydra/large_mul_cmp/128_cv                     1.69 %          1.69 %             3
hydra/large_mul_cmp/256                        32.3 ns         32.3 ns      8665779
hydra/large_mul_cmp/256                        32.3 ns         32.3 ns      8665779
hydra/large_mul_cmp/256                        32.1 ns         32.1 ns      8665779
hydra/large_mul_cmp/256_mean                   32.2 ns         32.2 ns            3
hydra/large_mul_cmp/256_median                 32.3 ns         32.3 ns            3
hydra/large_mul_cmp/256_stddev                0.120 ns        0.121 ns            3
hydra/large_mul_cmp/256_cv                     0.37 %          0.37 %             3
hydra/large_mul_cmp/512                        70.0 ns         69.9 ns      4000914
hydra/large_mul_cmp/512                        69.7 ns         69.7 ns      4000914
hydra/large_mul_cmp/512                        69.7 ns         69.7 ns      4000914
hydra/large_mul_cmp/512_mean                   69.8 ns         69.8 ns            3
hydra/large_mul_cmp/512_median                 69.7 ns         69.7 ns            3
hydra/large_mul_cmp/512_stddev                0.148 ns        0.147 ns            3
hydra/large_mul_cmp/512_cv                     0.21 %          0.21 %             3

── Comparison report ───────────────────────────────────────────────────
# Hydra Benchmark Report

Generated: `2026-04-15 13:11`  Machine: `Henrys-MacBook-Pro.local`  CPUs: `18 × 24 MHz`  Build: `release`

> Source: `bench_results.json`  Metric: `cpu_time`

### Small operations vs. native uint64_t
*Lower Δ is better. Goal: < 2× native for small ops. · metric: `cpu_time`*

| Operation | Subject (ns) | Reference (ns) | Ratio | Δ |
|-----------|-------------|----------------|-------|---|
| small add | 2.97 ns | 2.64 ns `baseline/u64_add` | 1.13× | +12.6% |
| small mul | 4.00 ns | 3.53 ns `baseline/u64_mul` | 1.13× | +13.4% |
| small sub (vs add) | 1.06 ns | 2.64 ns `baseline/u64_add` | 0.40× | -59.7% |
| widening add (vs native) | 3.16 ns | 2.64 ns `baseline/u64_add` | 1.20× | +19.5% |
| widening mul 128 (vs native) | 3.78 ns | 3.53 ns `baseline/u64_mul` | 1.07× | +7.2% |

### Medium / Large operations vs. Boost.Multiprecision
*Negative Δ means Hydra is faster than Boost. · metric: `cpu_time`*

| Operation | Subject (ns) | Reference (ns) | Ratio | Δ |
|-----------|-------------|----------------|-------|---|
| widening mul 128 | 3.78 ns | 9.18 ns `boost/widening_mul` | 0.41× | -58.8% |
| medium add (vs large/128) | 6.41 ns | 9.04 ns `boost/large_add/128` | 0.71× | -29.1% |
| medium mul (vs large/128) | 18.49 ns | 24.73 ns `boost/large_mul/128` | 0.75× | -25.2% |
| small add | 2.97 ns | 6.32 ns `boost/small_add` | 0.47× | -52.9% |
| small mul | 4.00 ns | 7.70 ns `boost/small_mul` | 0.52× | -48.0% |
| large add 128-bit | 18.27 ns | 9.04 ns `boost/large_add/128` | 2.02× | +102.2% |
| large add 256-bit | 2.65 µs | 21.41 ns `boost/large_add/256` | 123.75× | +12275.1% ⚠ |
| large add 512-bit | 2.63 µs | 21.70 ns `boost/large_add/512` | 121.37× | +12037.1% ⚠ |
| large mul 128-bit | 32.17 ns | 24.73 ns `boost/large_mul/128` | 1.30× | +30.1% |
| large mul 256-bit | 32.07 ns | 36.35 ns `boost/large_mul/256` | 0.88× | -11.8% |
| large mul 512-bit | 69.70 ns | 40.53 ns `boost/large_mul/512` | 1.72× | +71.9% |

  (skipped 1 pair(s) — benchmarks not in results)

### Allocation costs

| Benchmark | 4 limbs | 8 limbs | 16 limbs | 64 limbs | 256 limbs |
|-----------|------------|------------|------------|------------|------------|
| `alloc/from_limbs` | 12.08 ns | 9.81 ns | 10.04 ns | 20.40 ns | 28.65 ns |
| `alloc/largerep_clone` | 11.97 ns | — | 9.77 ns | 20.23 ns | 28.41 ns |
| `alloc/largerep_create_destroy` | 9.80 ns | — | 8.48 ns | 15.71 ns | 13.14 ns |

| Benchmark | Time |
|-----------|------|
| `alloc/normalize_large_to_medium` | 13.76 ns |
| `alloc/normalize_medium_to_small` | 1.55 ns |

### Copy / Move costs

| Benchmark | 4 limbs | 16 limbs | 64 limbs | 256 limbs |
|-----------|------------|------------|------------|------------|
| `copy/large` | 12.52 ns | 10.32 ns | 20.77 ns | 28.96 ns |
| `copy/move_large` | 1.24 ns | 1.24 ns | 1.24 ns | 1.24 ns |

| Benchmark | Time |
|-----------|------|
| `copy/medium` | 444.1 ps |
| `copy/move_medium` | 2.23 ns |
| `copy/small` | 324.8 ps |

### Arithmetic chain throughput

| Benchmark | 10 limbs | 20 limbs | 30 limbs | 50 limbs |
|-----------|------------|------------|------------|------------|
| `chain/factorial` | 17.52 ns | 67.03 ns | 130.15 ns | 400.12 ns |

| Benchmark | Time |
|-----------|------|
| `chain/small_add_10` | 31.46 ns |

---

**Note:** Some comparison pairs were skipped.
Run with `--benchmark_filter=all` or enable Boost benchmarks.

