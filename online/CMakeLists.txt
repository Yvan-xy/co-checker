cmake_minimum_required(VERSION 3.25)

project(co_checker_online VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CO_CHECKER_ONLINE_BUILD_TESTING "Build the online checker tests" ON)
option(CO_CHECKER_ONLINE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(CO_CHECKER_ONLINE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(CO_CHECKER_ONLINE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(CO_CHECKER_ONLINE_ENABLE_ASAN AND CO_CHECKER_ONLINE_ENABLE_TSAN)
  message(FATAL_ERROR "ASan and TSan cannot be enabled in the same build")
endif()

set(CO_CHECKER_ONLINE_SANITIZERS "")
if(CO_CHECKER_ONLINE_ENABLE_ASAN)
  list(APPEND CO_CHECKER_ONLINE_SANITIZERS address)
endif()
if(CO_CHECKER_ONLINE_ENABLE_UBSAN)
  list(APPEND CO_CHECKER_ONLINE_SANITIZERS undefined)
endif()
if(CO_CHECKER_ONLINE_ENABLE_TSAN)
  list(APPEND CO_CHECKER_ONLINE_SANITIZERS thread)
endif()

find_package(Threads REQUIRED)

add_library(co_checker_online
  src/engine.cpp
  src/order_gate.cpp
  src/packed_batch.cpp
  src/repeated_read_table.cpp
  src/runtime.cpp
  src/serialized_order.cpp
)
set_target_properties(co_checker_online PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

target_include_directories(co_checker_online
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(co_checker_online
  PUBLIC
    Threads::Threads
)

target_compile_options(co_checker_online
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
)

add_library(co_checker_online_c SHARED
  src/c_api.cpp
)
target_compile_definitions(co_checker_online_c PRIVATE
  CO_CHECKER_ONLINE_C_API_BUILD
)
target_include_directories(co_checker_online_c
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(co_checker_online_c PRIVATE co_checker_online)
target_compile_options(co_checker_online_c PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wsign-conversion
)
set_target_properties(co_checker_online_c PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)
if(UNIX AND NOT APPLE)
  target_link_options(co_checker_online_c PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/co_checker_online_c.map"
  )
endif()

add_executable(co_checker_online_trace_replay
  tools/trace_replay.cpp
)
target_link_libraries(co_checker_online_trace_replay PRIVATE co_checker_online)
target_compile_options(co_checker_online_trace_replay PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wsign-conversion
)

add_executable(co_checker_online_microbench
  tools/microbench.cpp
)
target_link_libraries(co_checker_online_microbench PRIVATE co_checker_online)
target_compile_options(co_checker_online_microbench PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wsign-conversion
)

add_executable(co_checker_online_c_api_bench
  tools/c_api_pipeline_bench.cpp
)
target_link_libraries(co_checker_online_c_api_bench PRIVATE
  co_checker_online_c
)
target_compile_options(co_checker_online_c_api_bench PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wsign-conversion
)

if(CO_CHECKER_ONLINE_SANITIZERS)
  list(JOIN CO_CHECKER_ONLINE_SANITIZERS "," CO_CHECKER_ONLINE_SANITIZER_LIST)
  target_compile_options(co_checker_online PUBLIC
    -fno-omit-frame-pointer
    -fsanitize=${CO_CHECKER_ONLINE_SANITIZER_LIST}
  )
  target_link_options(co_checker_online PUBLIC
    -fsanitize=${CO_CHECKER_ONLINE_SANITIZER_LIST}
  )
  target_compile_options(co_checker_online_c_api_bench PRIVATE
    -fno-omit-frame-pointer
    -fsanitize=${CO_CHECKER_ONLINE_SANITIZER_LIST}
  )
  target_link_options(co_checker_online_c_api_bench PRIVATE
    -fsanitize=${CO_CHECKER_ONLINE_SANITIZER_LIST}
  )
endif()

if(CO_CHECKER_ONLINE_BUILD_TESTING)
  include(CTest)
  find_package(Python3 COMPONENTS Interpreter)
  add_executable(co_checker_online_tests
    tests/c_api_header_smoke.c
    tests/reference_model.cpp
    tests/test_bounded_mpsc_queue.cpp
    tests/test_byte_budget.cpp
    tests/test_c_api.cpp
    tests/test_main.cpp
    tests/test_order_gate.cpp
    tests/test_packed_batch.cpp
    tests/test_repeated_read_table.cpp
    tests/test_runtime.cpp
    tests/test_serialized_order.cpp
  )
  target_link_libraries(co_checker_online_tests PRIVATE
    co_checker_online
    co_checker_online_c
  )
  target_compile_options(co_checker_online_tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
  )
  add_test(NAME co_checker_online_tests COMMAND co_checker_online_tests)
  if(UNIX)
    add_executable(co_checker_online_c_dlopen_test
      tests/test_c_api_dlopen.cpp
    )
    target_link_libraries(co_checker_online_c_dlopen_test PRIVATE
      ${CMAKE_DL_LIBS}
    )
    target_include_directories(co_checker_online_c_dlopen_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(co_checker_online_c_dlopen_test PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
    )
    if(CO_CHECKER_ONLINE_SANITIZERS)
      target_compile_options(co_checker_online_c_dlopen_test PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=${CO_CHECKER_ONLINE_SANITIZER_LIST}
      )
      target_link_options(co_checker_online_c_dlopen_test PRIVATE
        -fsanitize=${CO_CHECKER_ONLINE_SANITIZER_LIST}
      )
    endif()
    add_test(
      NAME co_checker_online_c_dlopen_test
      COMMAND co_checker_online_c_dlopen_test
              $<TARGET_FILE:co_checker_online_c>
    )
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME co_checker_online_layout_sampler_tests
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_layout_sampler.py
    )
    add_test(
      NAME co_checker_online_tpcc_global_lock_analyzer_tests
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_analyze_tpcc_global_lock.py
    )
  endif()
endif()
