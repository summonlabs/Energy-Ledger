set(ENERGY_LEDGER_EXAMPLES
  01_lifecycle
  02_residual_unclassified
  03_correction_history
  04_idempotent_replay
  05_stale_authority
  06_reopen_recovery)

foreach(example IN LISTS ENERGY_LEDGER_EXAMPLES)
  add_executable(example-${example} ${example}.cpp)
  target_link_libraries(example-${example} PRIVATE energy_ledger)
  target_include_directories(example-${example} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  energy_ledger_apply_warnings(example-${example})
  set_target_properties(example-${example} PROPERTIES OUTPUT_NAME "${example}")

  if(ENERGY_LEDGER_BUILD_TESTS)
    # Each example runs in its own scratch directory so the residue it creates
    # never mixes with the source tree. No test timeout is set.
    set(run_directory "${CMAKE_CURRENT_BINARY_DIR}/run-${example}")
    file(MAKE_DIRECTORY "${run_directory}")
    add_test(NAME example-${example} COMMAND example-${example} WORKING_DIRECTORY "${run_directory}")
  endif()
endforeach()
