add_library(el_test_support STATIC
  support/harness.cpp
  support/util.cpp
  support/process.cpp)
target_link_libraries(el_test_support PUBLIC energy_ledger)
target_include_directories(el_test_support PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(el_test_support PUBLIC
  EL_TEST_CLI_PATH="$<TARGET_FILE:energy-ledger>"
  EL_TEST_BENCH_PATH="$<TARGET_FILE:energy-ledger-bench>")
add_dependencies(el_test_support energy-ledger)
energy_ledger_apply_warnings(el_test_support)

# One executable per proof area. CTest runs each suite as a single test; the
# suite itself reports every case and never uses a timeout.
function(energy_ledger_add_suite name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE el_test_support)
  energy_ledger_apply_warnings(${name})
  add_dependencies(${name} energy-ledger)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

energy_ledger_add_suite(el_tests_core core_tests.cpp)
energy_ledger_add_suite(el_tests_store store_tests.cpp)
energy_ledger_add_suite(el_tests_accounting accounting_tests.cpp)
energy_ledger_add_suite(el_tests_process process_tests.cpp)
