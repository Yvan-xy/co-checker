cmake_minimum_required(VERSION 3.24)

project(co_checker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

add_library(co_checker
  src/checker/checker_registry.cpp
  src/checker/co_snapshot_checker.cpp
  src/diag/trace_dump.cpp
  src/trace/trace_index.cpp
  src/trace/trace_parser.cpp
  src/trace/types.cpp
)

target_include_directories(co_checker
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(co_checker
  PRIVATE
    yaml-cpp::yaml-cpp
)

target_compile_options(co_checker
  PRIVATE
    -Wall -Wextra -Wpedantic
)

add_executable(co-checker
  src/cli/main.cpp
)

target_link_libraries(co-checker
  PRIVATE
    co_checker
)

target_compile_options(co-checker
  PRIVATE
    -Wall -Wextra -Wpedantic
)

include(CTest)
if(BUILD_TESTING)
  add_executable(co_checker_tests
    test/test_main.cpp
  )

  target_link_libraries(co_checker_tests
    PRIVATE
      co_checker
  )

  target_compile_definitions(co_checker_tests
    PRIVATE
      CO_CHECKER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )

  target_compile_options(co_checker_tests
    PRIVATE
      -Wall -Wextra -Wpedantic
  )

  add_test(NAME co_checker_tests COMMAND co_checker_tests)
endif()
