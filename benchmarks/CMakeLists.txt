# Sequential -O3 baselines for each benchmark. The DSWP-transformed
# binaries are produced separately by scripts/run_transform.sh (which
# runs clang → opt → DSWPTransform → clang++ -O2 link). For the
# analyzer pass we only need .bc bitcode, also produced by scripts.

find_package(Threads REQUIRED)

foreach(b llist_sum llist_compute histogram fib_iter max_list llist_heavy llist_heavy_12 markov_chain_12)
  add_executable(${b}_seq ${b}.c)
  target_compile_options(${b}_seq PRIVATE -O3)
  if(${b} STREQUAL "llist_compute" OR ${b} STREQUAL "llist_heavy"
     OR ${b} STREQUAL "llist_heavy_12" OR ${b} STREQUAL "markov_chain_12")
    target_link_libraries(${b}_seq PRIVATE m)
  endif()
endforeach()
