cmake_minimum_required(VERSION 3.15)

# Suppress FindBoost deprecation warning (CMP0167)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(mito VERSION 0.7.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build options
option(COVERAGE "Enable code coverage reporting" OFF)

if(COVERAGE)
    message(STATUS "Code coverage enabled - building with --coverage -O0 -g")
    add_compile_options(--coverage -O0 -g -fno-inline -fprofile-update=atomic)
    add_link_options(--coverage)
    # Disable optimizations that interfere with coverage
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
else()
    add_compile_options(-O3)
endif()

# Always enable warnings
add_compile_options(-Wall -Wextra -Wpedantic)

# No architecture-specific flags. CRC32 comes from zlib-ng, which compiles its
# own SIMD paths with its own per-file flags and picks one at runtime, so
# raising the baseline ISA here would buy nothing and cost portability: the
# -march=armv8-a+crc this used to add made the binary emit CRC32 instructions
# that are optional before ARMv8.1, so it would SIGILL on an ARMv8.0 part rather
# than fall back (issue #49).

# Link-time optimization
include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported OUTPUT lto_error)
if(lto_supported AND NOT COVERAGE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Security hardening flags (release builds)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fstack-protector-strong)
    if(NOT APPLE)
        add_link_options(-Wl,-z,relro,-z,now)
    endif()
endif()

# Dependencies
find_package(AWSSDK REQUIRED COMPONENTS s3)
find_package(OpenSSL REQUIRED)

# Boost.Asio is header-only. Try CONFIG mode first (vcpkg), fall back to system.
find_package(Boost CONFIG COMPONENTS asio QUIET)
if(TARGET Boost_Asio)
    # vcpkg provides Boost_Asio target
    add_library(Boost_Asio ALIAS Boost_Asio)
else()
    # System Boost - create interface target for header-only Asio
    find_package(Boost REQUIRED)
    add_library(Boost_Asio INTERFACE)
    target_include_directories(Boost_Asio INTERFACE ${Boost_INCLUDE_DIRS})
endif()

find_package(spdlog CONFIG REQUIRED)

# zlib-ng supplies every CRC32 mito computes (issue #50). This is the prefixed
# zng_ API, not zlib-compat mode, so it coexists with the zlib that aws-sdk-cpp
# pulls in rather than replacing it triplet-wide.
#
# CONFIG mode finds the vcpkg package, which is how the presets and the release
# build resolve it. CI configures without the vcpkg toolchain, and no
# distribution CI runs on ships a zlib-ng CMake package (Ubuntu 24.04 has no
# libz-ng-dev at all), so fall back to building it from source the same way
# nlohmann_json does above. Either way the CRC32 code is identical and the
# implementation is still chosen from the CPU at run time.
set(MITO_ZLIB_NG_VERSION 2.2.4)
find_package(zlib-ng CONFIG QUIET)
if(zlib-ng_FOUND)
    set(MITO_ZLIB_NG_ORIGIN "find_package ${zlib-ng_VERSION}")
else()
    message(STATUS "zlib-ng not found via find_package, using FetchContent")
    include(FetchContent)
    # ZLIB_COMPAT off keeps the zng_ prefix, so this cannot shadow the zlib the
    # AWS SDK links. The test binaries are zlib-ng's own and not ours to run.
    set(ZLIB_COMPAT OFF CACHE BOOL "" FORCE)
    set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zlib-ng
        GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng.git
        GIT_TAG ${MITO_ZLIB_NG_VERSION}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(zlib-ng)
    set(MITO_ZLIB_NG_ORIGIN "FetchContent ${MITO_ZLIB_NG_VERSION}")

    # The installed package exports zlib-ng::zlib; an in-tree build does not, so
    # give it that name here rather than special-casing every target below.
    #
    # Which target to alias takes a moment's care. zlib-ng calls its library
    # `zlib`, and when BUILD_SHARED_LIBS is undefined it builds that one shared
    # and a separate static `zlibstatic` - aliasing `zlib` there would put a
    # libz-ng.so from the build tree on mito's runtime path. `zlibstatic` is the
    # static library in that case, and merely an alias for `zlib` when
    # BUILD_SHARED_LIBS is OFF; an alias cannot itself be aliased, so resolve
    # through it. With BUILD_SHARED_LIBS ON there is no static library at all
    # and `zlib` is the only choice.
    if(NOT TARGET zlib-ng::zlib)
        set(MITO_ZLIB_NG_TARGET zlib)
        if(TARGET zlibstatic)
            get_target_property(MITO_ZLIB_NG_ALIASED zlibstatic ALIASED_TARGET)
            if(MITO_ZLIB_NG_ALIASED)
                set(MITO_ZLIB_NG_TARGET ${MITO_ZLIB_NG_ALIASED})
            else()
                set(MITO_ZLIB_NG_TARGET zlibstatic)
            endif()
        endif()
        add_library(zlib-ng::zlib ALIAS ${MITO_ZLIB_NG_TARGET})
    endif()
endif()

# Plan step 3 of issue #50: say at configure time which library the CRC32 comes
# from, rather than leaving it to be inferred from the link line.
message(STATUS "CRC32 implementation: zlib-ng (${MITO_ZLIB_NG_ORIGIN})")

# nlohmann_json: try CONFIG mode first (vcpkg), fall back to FetchContent
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found via find_package, using FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# CLI application
set(SRC_FILES
    src/main.cpp
    src/cli_args.cpp
    src/report_writer.cpp
    src/s3_utils.cpp
    src/s3_client_impl.cpp
    src/crc32_chunks.cpp
    src/crc32_hw.cpp
    src/comparison_task.cpp
    src/directory_comparison.cpp
    src/url_parser.cpp
    src/dns_cache.cpp
    src/cached_dns_http_client.cpp
    src/app_settings.cpp
    src/cloud_metrics.cpp
    src/cloud_monitoring.cpp
    src/aws_pricing.cpp
    src/sync_task.cpp
    src/rm_task.cpp
    src/leftovers_task.cpp
    src/duration_parse.cpp
)

add_executable(${PROJECT_NAME} ${SRC_FILES})
target_include_directories(${PROJECT_NAME} PRIVATE include)
target_link_libraries(${PROJECT_NAME}
    aws-cpp-sdk-s3
    OpenSSL::SSL
    OpenSSL::Crypto
    Boost_Asio
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    zlib-ng::zlib
)

# The version `mito --version` reports comes from project() above, so the binary, the
# artifact name and vcpkg.json cannot drift apart without someone noticing.
# scripts/package-release.sh checks CMakeLists.txt against vcpkg.json for the same reason.
target_compile_definitions(${PROJECT_NAME} PRIVATE MITO_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Testing
enable_testing()
find_package(GTest CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

# CRC32 tests
add_executable(mito_tests_crc32
    tests/test_crc32.cpp
    src/crc32_hw.cpp
)
target_include_directories(mito_tests_crc32 PRIVATE include)
target_link_libraries(mito_tests_crc32
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    zlib-ng::zlib
)

# CRC32 tests built for the host's full x86 instruction set.
#
# Since crc32_hw.cpp delegates to zlib-ng there is no ISA-gated code left in
# mito for -march=native to switch on, so this no longer wakes a dormant path.
# It is kept as the guard that would catch one being reintroduced: a CRC that
# only misbehaves when the compiler is allowed to use the host's instruction
# set is exactly the shape of issue #18, and the default build would not run it.
#
# x86 only, deliberately: -march=native is the flag with the dormancy history
# there, and on ARM the last -march wins, so appending it would silently
# redefine the target architecture for this target.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-march=native" HAVE_MARCH_NATIVE)
endif()
if(HAVE_MARCH_NATIVE)
    add_executable(mito_tests_crc32_native
        tests/test_crc32.cpp
        src/crc32_hw.cpp
    )
    target_include_directories(mito_tests_crc32_native PRIVATE include)
    target_compile_options(mito_tests_crc32_native PRIVATE -march=native)
    target_link_libraries(mito_tests_crc32_native
        GTest::gtest
        GTest::gtest_main
        ZLIB::ZLIB
        zlib-ng::zlib
    )
endif()

# CRC32 benchmark (issue #50, plan step 5). Built like everything else so it
# cannot rot, but deliberately not registered with ctest: it measures rather
# than asserts, and it takes seconds rather than milliseconds. Run it by hand
# with ./build/mito_bench_crc32.
add_executable(mito_bench_crc32
    tests/bench_crc32.cpp
    src/crc32_hw.cpp
)
target_include_directories(mito_bench_crc32 PRIVATE include)
target_link_libraries(mito_bench_crc32
    ZLIB::ZLIB
    zlib-ng::zlib
)

# URL parser tests
add_executable(mito_tests_url_parser
    tests/test_url_parser.cpp
    src/url_parser.cpp
)
target_include_directories(mito_tests_url_parser PRIVATE include)
target_link_libraries(mito_tests_url_parser
    GTest::gtest
    GTest::gtest_main
)

# File utilities tests (includes local CRC and comparison tests)
add_executable(mito_tests_file_utils
    tests/test_file_utils.cpp
    src/s3_utils.cpp
    src/crc32_hw.cpp
    src/crc32_chunks.cpp
    src/s3_client_impl.cpp
    src/comparison_task.cpp
    src/directory_comparison.cpp
    src/dns_cache.cpp
    src/cached_dns_http_client.cpp
    src/app_settings.cpp
)
target_include_directories(mito_tests_file_utils PRIVATE include)
target_link_libraries(mito_tests_file_utils
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    spdlog::spdlog
    Boost_Asio
    zlib-ng::zlib
)

# S3 mock tests (integration tests using mock S3 client)
add_executable(mito_tests_s3_mock
    tests/test_s3_mock.cpp
    src/crc32_hw.cpp
    src/crc32_chunks.cpp
    src/s3_utils.cpp
    src/s3_client_impl.cpp
    src/comparison_task.cpp
    src/directory_comparison.cpp
    src/dns_cache.cpp
    src/cached_dns_http_client.cpp
    src/app_settings.cpp
)
target_include_directories(mito_tests_s3_mock PRIVATE include)
target_link_libraries(mito_tests_s3_mock
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    spdlog::spdlog
    Boost_Asio
    zlib-ng::zlib
)

# Utility tests (bucket listing, AWS credential parsing)
add_executable(mito_tests_utils
    tests/test_bucket_list.cpp
    tests/test_credentials_parser.cpp
)
target_include_directories(mito_tests_utils PRIVATE include)
target_link_libraries(mito_tests_utils
    GTest::gtest
    GTest::gtest_main
)

# Sync type tests (direction detection and config construction)
add_executable(mito_tests_sync_types
    tests/test_sync_types.cpp
    src/url_parser.cpp
)
target_include_directories(mito_tests_sync_types PRIVATE include)
target_link_libraries(mito_tests_sync_types
    GTest::gtest
    GTest::gtest_main
)

# Directory comparison tests
add_executable(mito_tests_directory
    tests/test_directory_comparison.cpp
    src/crc32_hw.cpp
    src/crc32_chunks.cpp
    src/s3_utils.cpp
    src/s3_client_impl.cpp
    src/comparison_task.cpp
    src/directory_comparison.cpp
    src/dns_cache.cpp
    src/cached_dns_http_client.cpp
    src/app_settings.cpp
)
target_include_directories(mito_tests_directory PRIVATE include)
target_link_libraries(mito_tests_directory
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    spdlog::spdlog
    Boost_Asio
    zlib-ng::zlib
)

# Combined test target (all tests)
add_executable(mito_tests
    tests/test_crc32.cpp
    tests/test_url_parser.cpp
    tests/test_file_utils.cpp
    tests/test_s3_mock.cpp
    tests/test_bucket_list.cpp
    tests/test_credentials_parser.cpp
    tests/test_directory_comparison.cpp
    src/crc32_hw.cpp
    src/crc32_chunks.cpp
    src/url_parser.cpp
    src/s3_utils.cpp
    src/s3_client_impl.cpp
    src/comparison_task.cpp
    src/directory_comparison.cpp
    src/dns_cache.cpp
    src/cached_dns_http_client.cpp
    src/app_settings.cpp
)
target_include_directories(mito_tests PRIVATE include)
target_link_libraries(mito_tests
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    spdlog::spdlog
    Boost_Asio
    zlib-ng::zlib
)

# Cloud metrics tests
add_executable(mito_tests_cloud_metrics
    tests/test_cloud_metrics.cpp
    src/cloud_metrics.cpp
    src/app_settings.cpp
)
target_include_directories(mito_tests_cloud_metrics PRIVATE include)
target_link_libraries(mito_tests_cloud_metrics
    GTest::gtest
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# AWS pricing tests
add_executable(mito_tests_aws_pricing
    tests/test_aws_pricing.cpp
    src/aws_pricing.cpp
    src/cloud_metrics.cpp
    src/app_settings.cpp
)
target_include_directories(mito_tests_aws_pricing PRIVATE include)
target_link_libraries(mito_tests_aws_pricing
    GTest::gtest
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# Cloud monitoring tests (requires AWS SDK)
add_executable(mito_tests_cloud_monitoring
    tests/test_cloud_monitoring.cpp
    src/cloud_monitoring.cpp
    src/cloud_metrics.cpp
    src/app_settings.cpp
)
target_include_directories(mito_tests_cloud_monitoring PRIVATE include)
target_link_libraries(mito_tests_cloud_monitoring
    GTest::gtest
    GTest::gtest_main
    aws-cpp-sdk-core
    aws-cpp-sdk-s3
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# Sync tests
add_executable(mito_tests_sync
    tests/test_sync.cpp
    src/sync_task.cpp
    src/crc32_hw.cpp
    src/crc32_chunks.cpp
    src/s3_utils.cpp
    src/s3_client_impl.cpp
    src/comparison_task.cpp
    src/directory_comparison.cpp
    src/dns_cache.cpp
    src/cached_dns_http_client.cpp
    src/app_settings.cpp
    src/cloud_metrics.cpp
)
target_include_directories(mito_tests_sync PRIVATE include)
target_link_libraries(mito_tests_sync
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    spdlog::spdlog
    Boost_Asio
    nlohmann_json::nlohmann_json
    zlib-ng::zlib
)

# Integration tests against a live S3-compatible endpoint.
#
# Built always so it cannot rot, but every test skips itself unless
# MITO_TEST_S3_ENDPOINT is set - see scripts/s3-emulator.sh. Registered under
# the s3-integration label so it can be selected with `ctest -L s3-integration`
# or excluded with `ctest -LE s3-integration`.
add_executable(mito_tests_s3_integration
    tests/test_s3_integration.cpp
    src/sync_task.cpp
    src/rm_task.cpp
    src/leftovers_task.cpp
    src/crc32_hw.cpp
    src/crc32_chunks.cpp
    src/s3_utils.cpp
    src/s3_client_impl.cpp
    src/comparison_task.cpp
    src/directory_comparison.cpp
    src/dns_cache.cpp
    src/cached_dns_http_client.cpp
    src/app_settings.cpp
    src/cloud_metrics.cpp
)
target_include_directories(mito_tests_s3_integration PRIVATE include)
target_link_libraries(mito_tests_s3_integration
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    spdlog::spdlog
    Boost_Asio
    nlohmann_json::nlohmann_json
    zlib-ng::zlib
)

# Rm tests
add_executable(mito_tests_rm
    tests/test_rm.cpp
    src/rm_task.cpp
    src/crc32_hw.cpp
    src/crc32_chunks.cpp
    src/s3_utils.cpp
    src/s3_client_impl.cpp
    src/comparison_task.cpp
    src/directory_comparison.cpp
    src/dns_cache.cpp
    src/cached_dns_http_client.cpp
    src/app_settings.cpp
)
target_include_directories(mito_tests_rm PRIVATE include)
target_link_libraries(mito_tests_rm
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    spdlog::spdlog
    Boost_Asio
    zlib-ng::zlib
)

# Leftovers tests
add_executable(mito_tests_leftovers
    tests/test_leftovers.cpp
    src/leftovers_task.cpp
    src/duration_parse.cpp
    src/crc32_hw.cpp
    src/crc32_chunks.cpp
    src/s3_utils.cpp
    src/s3_client_impl.cpp
    src/dns_cache.cpp
    src/cached_dns_http_client.cpp
    src/app_settings.cpp
)
target_link_libraries(mito_tests_leftovers PRIVATE
    GTest::gtest_main
    spdlog::spdlog
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    Boost_Asio
    zlib-ng::zlib
)
target_include_directories(mito_tests_leftovers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Real S3 adapter tests (fake SDK client injected, no network)
add_executable(mito_tests_s3_client_impl
    tests/test_s3_client_impl.cpp
    src/s3_client_impl.cpp
    src/s3_utils.cpp
    src/crc32_chunks.cpp
    src/crc32_hw.cpp
    src/app_settings.cpp
)
target_include_directories(mito_tests_s3_client_impl PRIVATE include)
target_link_libraries(mito_tests_s3_client_impl
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    spdlog::spdlog
    Boost_Asio
    zlib-ng::zlib
)

# S3 utility tests (network-free helpers: jitter, retry policy, file size, upload registry)
add_executable(mito_tests_s3_utils
    tests/test_s3_utils.cpp
    src/s3_utils.cpp
)
target_include_directories(mito_tests_s3_utils PRIVATE include)
target_link_libraries(mito_tests_s3_utils
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    spdlog::spdlog
    Boost_Asio
)

# Report writer / formatting tests
add_executable(mito_tests_report_writer
    tests/test_report_writer.cpp
    src/report_writer.cpp
)
target_include_directories(mito_tests_report_writer PRIVATE include)
target_link_libraries(mito_tests_report_writer
    GTest::gtest
    GTest::gtest_main
)

# CLI argument parsing tests
add_executable(mito_tests_cli_args
    tests/test_cli_args.cpp
    src/cli_args.cpp
    src/url_parser.cpp
    src/sync_task.cpp
    src/crc32_hw.cpp
    src/crc32_chunks.cpp
    src/s3_utils.cpp
    src/s3_client_impl.cpp
    src/comparison_task.cpp
    src/directory_comparison.cpp
    src/dns_cache.cpp
    src/cached_dns_http_client.cpp
    src/app_settings.cpp
    src/cloud_metrics.cpp
)
target_include_directories(mito_tests_cli_args PRIVATE include)
target_link_libraries(mito_tests_cli_args
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    spdlog::spdlog
    Boost_Asio
    nlohmann_json::nlohmann_json
    zlib-ng::zlib
)

# Byte provider tests
add_executable(mito_tests_byte_provider
    tests/test_byte_provider.cpp
    src/byte_provider.cpp
    src/s3_client_impl.cpp
    src/s3_utils.cpp
    src/crc32_hw.cpp
    src/crc32_chunks.cpp
    src/dns_cache.cpp
    src/cached_dns_http_client.cpp
    src/app_settings.cpp
)
target_include_directories(mito_tests_byte_provider PRIVATE include)
target_link_libraries(mito_tests_byte_provider
    GTest::gtest
    GTest::gtest_main
    ZLIB::ZLIB
    aws-cpp-sdk-s3
    spdlog::spdlog
    Boost_Asio
    zlib-ng::zlib
)

include(GoogleTest)
gtest_discover_tests(mito_tests_cli_args)
gtest_discover_tests(mito_tests_report_writer)
gtest_discover_tests(mito_tests_s3_utils)
gtest_discover_tests(mito_tests_s3_client_impl)
gtest_discover_tests(mito_tests_byte_provider)
gtest_discover_tests(mito_tests_rm)
gtest_discover_tests(mito_tests_leftovers)
gtest_discover_tests(mito_tests_sync)
gtest_discover_tests(mito_tests_crc32)
if(HAVE_MARCH_NATIVE)
    gtest_discover_tests(mito_tests_crc32_native)
endif()
gtest_discover_tests(mito_tests_url_parser)
gtest_discover_tests(mito_tests_file_utils)
gtest_discover_tests(mito_tests_s3_mock)
gtest_discover_tests(mito_tests_utils)
gtest_discover_tests(mito_tests_sync_types)
gtest_discover_tests(mito_tests_directory)
gtest_discover_tests(mito_tests)
gtest_discover_tests(mito_tests_cloud_metrics)
# Labelled so a default run can exclude them and a deliberate run can select
# them. They skip themselves without MITO_TEST_S3_ENDPOINT, so the label is for
# intent rather than for keeping the default suite green.
gtest_discover_tests(mito_tests_s3_integration PROPERTIES LABELS s3-integration)
gtest_discover_tests(mito_tests_aws_pricing)
gtest_discover_tests(mito_tests_cloud_monitoring)

# End-to-end CLI test. main.cpp is compiled only into the mito executable and
# the command handlers are static, so nothing in the gtest suite can reach
# them; this drives the built binary instead. See issue #44 for the broader
# gap this is a first instance of.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME CliLeftoversPreservesAccumulatedMetrics
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/leftovers_preserves_metrics.py
                     $<TARGET_FILE:mito>)
    set_tests_properties(CliLeftoversPreservesAccumulatedMetrics PROPERTIES TIMEOUT 300)

    add_test(NAME CliConcurrentMetricsSavesDoNotCorruptTheFile
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/concurrent_metrics_saves.py
                     $<TARGET_FILE:mito>)
    set_tests_properties(CliConcurrentMetricsSavesDoNotCorruptTheFile PROPERTIES TIMEOUT 600)
else()
    message(STATUS "Python3 not found: skipping the CLI end-to-end tests")
endif()
