add_executable(energy-ledger-bench main.cpp)
target_link_libraries(energy-ledger-bench PRIVATE energy_ledger)
energy_ledger_apply_warnings(energy-ledger-bench)
set_target_properties(energy-ledger-bench PROPERTIES OUTPUT_NAME "energy-ledger-bench")

if(ENERGY_LEDGER_BUILD_TESTS)
  # A short benchmark run proves the measured path completes and cleans up after
  # itself. The published benchmark numbers come from a deliberate sized run.
  set(bench_directory "${CMAKE_CURRENT_BINARY_DIR}/run-bench")
  file(MAKE_DIRECTORY "${bench_directory}")
  add_test(NAME benchmark-smoke
    COMMAND energy-ledger-bench --entries 200 --rollup-queries 20 --reopen-repetitions 1
    WORKING_DIRECTORY "${bench_directory}")
endif()
