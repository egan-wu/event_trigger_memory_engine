cmake_minimum_required(VERSION 3.15)
project(ddrtiming LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Static-link the runtime so the built binaries don't depend on finding a
# matching libstdc++/libgcc DLL at load time (a real hazard on Windows with
# multiple MinGW toolchains on PATH) -- also makes them more portable to drop
# into other environments, which is the point of shipping this as a binary.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_link_options(-static-libgcc -static-libstdc++ -static)
endif()

set(CORE_SOURCES
    src/core/json.cpp
    src/core/config.cpp
    src/core/address_decoder.cpp
    src/core/command_queue.cpp
    src/core/engine.cpp
    src/core/log_parser.cpp
    src/core/report.cpp
)

add_library(ddrtiming STATIC ${CORE_SOURCES} src/api/ddrtiming_api.cpp)
target_include_directories(ddrtiming PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(ddrtiming_cli src/cli/main.cpp)
target_link_libraries(ddrtiming_cli PRIVATE ddrtiming)

add_executable(daemon_demo examples/daemon_demo.c)
target_link_libraries(daemon_demo PRIVATE ddrtiming)

add_executable(windowed_history_analyze tools/windowed_history_analyze.cpp)
target_link_libraries(windowed_history_analyze PRIVATE ddrtiming)

# bench/ regression harness -- see bench/README.md and golden_check.cpp's
# own header comment for what this checks and why.
add_executable(golden_check tools/golden_check.cpp)
target_link_libraries(golden_check PRIVATE ddrtiming)

enable_testing()
add_executable(ddrtiming_tests
    tests/test_main.cpp
    tests/test_address_decoder.cpp
    tests/test_engine_basic.cpp
    tests/test_command_queue.cpp
    tests/test_engine_stress.cpp
    tests/test_concurrency.cpp
    tests/test_config_validation.cpp
)
target_include_directories(ddrtiming_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
find_package(Threads REQUIRED)
target_link_libraries(ddrtiming_tests PRIVATE ddrtiming Threads::Threads)
add_test(NAME ddrtiming_tests COMMAND ddrtiming_tests)

add_test(NAME windowed_history_analyze_smoke
         COMMAND windowed_history_analyze --csv ${CMAKE_CURRENT_SOURCE_DIR}/examples/windowed_history_imbalance.example.csv)
set_tests_properties(windowed_history_analyze_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "avg_bandwidth_gbps")

add_test(NAME windowed_history_analyze_baseline_smoke
         COMMAND windowed_history_analyze
                 --csv ${CMAKE_CURRENT_SOURCE_DIR}/examples/windowed_history_imbalance.example.csv
                 --baseline ${CMAKE_CURRENT_SOURCE_DIR}/examples/windowed_history_imbalance.example.csv)
set_tests_properties(windowed_history_analyze_baseline_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "baseline_delta")

# bench/ golden-snapshot regression check -- see bench/README.md. Goldens
# currently capture known-incorrect timing-model behavior on purpose (see
# that README's warning); this test's job is to catch *unintended* drift
# between runs, not to assert the numbers are physically correct.
add_test(NAME golden_check
         COMMAND golden_check --bench-root ${CMAKE_CURRENT_SOURCE_DIR}/bench)
