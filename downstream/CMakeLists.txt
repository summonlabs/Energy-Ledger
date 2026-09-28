cmake_minimum_required(VERSION 3.20)

# Independent out-of-tree consumer. This project is deliberately not part of the
# Energy Ledger build: it exists to prove that the installed package can be
# consumed through find_package with no knowledge of the source tree.
project(EnergyLedgerDownstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(EnergyLedger 1.0 REQUIRED)

add_executable(energy-ledger-downstream main.cpp)
target_link_libraries(energy-ledger-downstream PRIVATE Summon::EnergyLedger)

if(MSVC)
  target_compile_options(energy-ledger-downstream PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(energy-ledger-downstream PRIVATE -Wall -Wextra -Wpedantic)
endif()

message(STATUS "Energy Ledger package version: ${ENERGY_LEDGER_VERSION}")
