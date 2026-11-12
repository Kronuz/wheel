cmake_minimum_required(VERSION 3.14)
project(wheel LANGUAGES CXX)

# --- Library target ---
# Header-only: wheel.hh is the whole implementation. The target exists for the include path and
# the C++ standard requirement.
add_library(wheel INTERFACE)
target_include_directories(wheel INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(wheel INTERFACE cxx_std_20)

# Alias so consumers can link `wheel::wheel` whether they add this via add_subdirectory or
# FetchContent.
add_library(wheel::wheel ALIAS wheel)

# Only build the tests, demo and bench when this repo is the top-level project, so consumers
# (e.g. via FetchContent) don't build them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  find_package(Threads REQUIRED)

  add_executable(wheel_test test/wheel_test.cc)
  target_link_libraries(wheel_test PRIVATE wheel Threads::Threads)
  add_test(NAME wheel COMMAND wheel_test)

  add_executable(wheel_alloc_test test/wheel_alloc_test.cc)
  target_link_libraries(wheel_alloc_test PRIVATE wheel Threads::Threads)
  add_test(NAME wheel_alloc COMMAND wheel_alloc_test)

  # libFuzzer target (Clang): cmake -DWHEEL_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
  option(WHEEL_FUZZ "Build the libFuzzer target" OFF)
  if(WHEEL_FUZZ)
    add_executable(wheel_fuzz fuzz/fuzz_ops.cc)
    target_link_libraries(wheel_fuzz PRIVATE wheel)
    target_compile_options(wheel_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_options(wheel_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  endif()

  # Detent's wheel-evidence rows (bench/wheel_bench.sh builds its own -O2 -DNDEBUG binary);
  # this target only keeps the harness compiling.
  add_executable(wheel_bench bench/wheel_bench.cc)
  target_include_directories(wheel_bench PRIVATE bench)
  target_link_libraries(wheel_bench PRIVATE wheel Threads::Threads)
  target_compile_options(wheel_bench PRIVATE -O2)

  # A runnable tour of the library (not a test): build & run ./build/wheel_demo
  add_executable(wheel_demo examples/demo.cc)
  target_link_libraries(wheel_demo PRIVATE wheel Threads::Threads)
  add_test(NAME wheel_demo COMMAND wheel_demo)
endif()
