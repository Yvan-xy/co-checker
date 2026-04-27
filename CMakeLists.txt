cmake_minimum_required(VERSION 3.28)

project(checker VERSION 0.1.0 LANGUAGES CXX)

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

FetchContent_Declare(
  immer
  GIT_REPOSITORY https://github.com/arximboldi/immer.git
  GIT_TAG bd4fc749b97dfa2b66a8f3de00bbf234db4856ef
)

# yaml-cpp 0.8.0 has not updated its declared policy floor for CMake 4.x.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(yaml-cpp immer)

if(TARGET immer)
  get_target_property(IMMER_INCLUDE_DIRS immer INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(immer PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${IMMER_INCLUDE_DIRS}"
  )
endif()

add_library(checker
  src/checker/check_result.cpp
  src/checker/checker_registry.cpp
  src/checker/co_snapshot_checker.cpp
  src/checker/so_only_checker.cpp
  src/diag/trace_dump.cpp
  src/trace/trace.cpp
  src/trace/trace_index.cpp
  src/trace/trace_parser.cpp
  src/trace/types.cpp
)

target_include_directories(checker
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(checker
  PUBLIC
    immer
  PRIVATE
    yaml-cpp::yaml-cpp
)

target_compile_options(checker
  PRIVATE
    -Wall -Wextra -Wpedantic
)

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)

file(GLOB_RECURSE CHECKER_FORMAT_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/test/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/test/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp"
)

if(CLANG_FORMAT_EXECUTABLE)
  add_custom_target(checker-format
    COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${CHECKER_FORMAT_FILES}
    COMMENT "Formatting C++ sources with clang-format"
    VERBATIM
  )

  add_custom_target(checker-format-check
    COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror
            ${CHECKER_FORMAT_FILES}
    COMMENT "Checking C++ source formatting with clang-format"
    VERBATIM
  )
else()
  add_custom_target(checker-format
    COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found"
    COMMAND ${CMAKE_COMMAND} -E false
    VERBATIM
  )

  add_custom_target(checker-format-check
    COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found"
    COMMAND ${CMAKE_COMMAND} -E false
    VERBATIM
  )
endif()

if(TARGET format)
  add_dependencies(format checker-format)
else()
  add_custom_target(format DEPENDS checker-format)
endif()

if(TARGET format-check)
  add_dependencies(format-check checker-format-check)
else()
  add_custom_target(format-check DEPENDS checker-format-check)
endif()

add_executable(co-checker
  src/cli/main.cpp
)

target_link_libraries(co-checker
  PRIVATE
    checker
)

target_compile_options(co-checker
  PRIVATE
    -Wall -Wextra -Wpedantic
)

include(CTest)
if(BUILD_TESTING)
  add_executable(checker_tests
    test/test_checker.cpp
    test/test_harness.cpp
    test/test_index.cpp
    test/test_main.cpp
    test/test_snapshot.cpp
    test/test_trace.cpp
  )

  target_link_libraries(checker_tests
    PRIVATE
      checker
  )

  target_compile_definitions(checker_tests
    PRIVATE
      CO_CHECKER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )

  target_compile_options(checker_tests
    PRIVATE
      -Wall -Wextra -Wpedantic
  )

  add_test(NAME checker_tests COMMAND checker_tests)
endif()
