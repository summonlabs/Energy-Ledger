cmake_minimum_required(VERSION 3.20)

project(EnergyLedger
  VERSION 1.0.0
  DESCRIPTION "Durable, provenance-preserving accounting ledger for facility electrical energy"
  HOMEPAGE_URL "https://github.com/summonlabs/Energy-Ledger"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(ENERGY_LEDGER_BUILD_CLI "Build the energy-ledger administration CLI" ON)
option(ENERGY_LEDGER_BUILD_EXAMPLES "Build the example programs" ON)
option(ENERGY_LEDGER_BUILD_BENCHMARKS "Build the benchmark executable" ON)
option(ENERGY_LEDGER_BUILD_TESTS "Build the automated test suite" ON)
option(ENERGY_LEDGER_STRICT_WARNINGS "Promote first-party warnings to errors" ON)
option(ENERGY_LEDGER_ENABLE_ASAN "Build with AddressSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# First-party warning policy. Warnings are never suppressed globally.
# ---------------------------------------------------------------------------
function(energy_ledger_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    if(ENERGY_LEDGER_STRICT_WARNINGS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wold-style-cast)
    if(ENERGY_LEDGER_STRICT_WARNINGS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(ENERGY_LEDGER_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(energy_ledger
  src/sha256.cpp
  src/crc32.cpp
  src/strong.cpp
  src/units.cpp
  src/entry.cpp
  src/canonical.cpp
  src/json.cpp
  src/os_file.cpp
  src/store.cpp
  src/ledger.cpp
  src/query.cpp
  src/test_support.cpp)
add_library(Summon::EnergyLedger ALIAS energy_ledger)

configure_file(
  include/energy_ledger/version.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/energy_ledger/version.hpp"
  @ONLY)

target_include_directories(energy_ledger
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

set_target_properties(energy_ledger PROPERTIES
  OUTPUT_NAME "energy_ledger"
  EXPORT_NAME "EnergyLedger"
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

energy_ledger_apply_warnings(energy_ledger)

# ---------------------------------------------------------------------------
# CLI, examples, benchmarks, tests
# ---------------------------------------------------------------------------
if(ENERGY_LEDGER_BUILD_TESTS)
  enable_testing()
endif()

if(ENERGY_LEDGER_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(ENERGY_LEDGER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(ENERGY_LEDGER_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
if(ENERGY_LEDGER_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS energy_ledger
  EXPORT EnergyLedgerTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/energy_ledger
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")

# The version header is generated from the project version, so it is installed
# separately from the source headers.
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/energy_ledger/version.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/energy_ledger)

install(EXPORT EnergyLedgerTargets
  FILE EnergyLedgerTargets.cmake
  NAMESPACE Summon::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EnergyLedger)

configure_package_config_file(
  cmake/EnergyLedgerConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/EnergyLedgerConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EnergyLedger)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/EnergyLedgerConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/EnergyLedgerConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/EnergyLedgerConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EnergyLedger)

install(FILES README.md LICENSE NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/EnergyLedger)
