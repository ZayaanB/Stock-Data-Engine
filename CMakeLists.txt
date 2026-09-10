cmake_minimum_required(VERSION 3.20)
project(itch_order_book VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(ITCH_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(ITCH_ENABLE_TSAN "Enable the thread sanitizer" OFF)
option(ITCH_NATIVE_ARCH "Build optimized binaries for the current CPU" OFF)
option(ITCH_FETCH_DEPS "Fetch GoogleTest and Google Benchmark" OFF)
option(ITCH_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)

if(ITCH_FETCH_DEPS)
  include(FetchContent)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest googlebenchmark)
endif()

function(itch_enable_warnings target)
  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)
endfunction()

add_library(itch_engine
    src/moldudp64.cpp
    src/parser.cpp
    src/pcap.cpp
    src/order_book.cpp
    src/replay.cpp)
add_library(ItchOrderBook::Engine ALIAS itch_engine)
set_target_properties(itch_engine PROPERTIES EXPORT_NAME Engine)
target_include_directories(
  itch_engine
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_features(itch_engine PUBLIC cxx_std_23)
itch_enable_warnings(itch_engine)
find_package(Threads REQUIRED)
target_link_libraries(itch_engine PUBLIC Threads::Threads)

if(ITCH_NATIVE_ARCH)
  target_compile_options(itch_engine PUBLIC -march=native)
endif()
if(ITCH_ENABLE_SANITIZERS AND ITCH_ENABLE_TSAN)
  message(FATAL_ERROR "Address/undefined sanitizers cannot be combined with ThreadSanitizer")
elseif(ITCH_ENABLE_SANITIZERS)
  target_compile_options(itch_engine PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(itch_engine PUBLIC -fsanitize=address,undefined)
elseif(ITCH_ENABLE_TSAN)
  target_compile_options(itch_engine PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(itch_engine PUBLIC -fsanitize=thread)
endif()

add_executable(itch_order_book src/main.cpp)
target_link_libraries(itch_order_book PRIVATE itch_engine)
itch_enable_warnings(itch_order_book)

add_executable(generate_feed tools/generate_feed.cpp)
target_link_libraries(generate_feed PRIVATE itch_engine)
itch_enable_warnings(generate_feed)

add_executable(book_benchmark benchmarks/book_benchmark.cpp)
target_link_libraries(book_benchmark PRIVATE itch_engine)
itch_enable_warnings(book_benchmark)

add_executable(lookup_policy_benchmark benchmarks/lookup_policy_benchmark.cpp)
itch_enable_warnings(lookup_policy_benchmark)

# Use the standard Google Benchmark runner when the package is available. The
# dependency-free benchmark above remains available on minimal/offline systems.
find_package(benchmark QUIET)
if(TARGET benchmark::benchmark)
  add_executable(google_book_benchmark benchmarks/google_book_benchmark.cpp)
  target_link_libraries(google_book_benchmark PRIVATE itch_engine benchmark::benchmark)
  itch_enable_warnings(google_book_benchmark)
endif()

enable_testing()
add_executable(itch_tests tests/tests.cpp)
target_link_libraries(itch_tests PRIVATE itch_engine)
itch_enable_warnings(itch_tests)
add_test(NAME itch_tests COMMAND itch_tests)

find_package(GTest QUIET)
if(TARGET GTest::gtest_main)
  add_executable(itch_gtests tests/google_tests.cpp)
  target_link_libraries(itch_gtests PRIVATE itch_engine GTest::gtest_main)
  itch_enable_warnings(itch_gtests)
  include(GoogleTest)
  gtest_discover_tests(itch_gtests)
endif()

if(ITCH_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ITCH_BUILD_FUZZERS requires Clang")
  endif()
  target_compile_options(
    itch_engine PRIVATE -fsanitize=fuzzer-no-link,address,undefined
                        -fno-omit-frame-pointer)
  target_link_options(itch_engine INTERFACE -fsanitize=address,undefined)
  add_executable(itch_decoder_fuzzer fuzz/fuzz_decoders.cpp)
  target_link_libraries(itch_decoder_fuzzer PRIVATE itch_engine)
  target_compile_options(
    itch_decoder_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined
                                  -fno-omit-frame-pointer)
  target_link_options(itch_decoder_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
  itch_enable_warnings(itch_decoder_fuzzer)
endif()

install(
  TARGETS itch_engine
  EXPORT itch_order_bookTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(
  TARGETS itch_order_book generate_feed book_benchmark lookup_policy_benchmark
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

include(CMakePackageConfigHelpers)
set(itch_order_book_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/itch_order_book")
configure_package_config_file(
  cmake/itch_order_bookConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/itch_order_bookConfig.cmake"
  INSTALL_DESTINATION "${itch_order_book_cmake_dir}")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/itch_order_bookConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(
  EXPORT itch_order_bookTargets
  FILE itch_order_bookTargets.cmake
  NAMESPACE ItchOrderBook::
  DESTINATION "${itch_order_book_cmake_dir}")
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/itch_order_bookConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/itch_order_bookConfigVersion.cmake"
  DESTINATION "${itch_order_book_cmake_dir}")
