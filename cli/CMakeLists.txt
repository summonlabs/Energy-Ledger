add_executable(energy-ledger main.cpp)
target_link_libraries(energy-ledger PRIVATE energy_ledger)
energy_ledger_apply_warnings(energy-ledger)
set_target_properties(energy-ledger PROPERTIES OUTPUT_NAME "energy-ledger")

install(TARGETS energy-ledger RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
