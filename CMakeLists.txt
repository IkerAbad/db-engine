cmake_minimum_required(VERSION 3.25)
project(db_engine VERSION 0.2.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wpointer-arith)
endif()

if(MINGW)
  add_link_options(-static-libgcc -static-libstdc++)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(_GLIBCXX_ASSERTIONS)
endif()

add_library(dbengine_core STATIC
  src/buffer_pool.cpp
  src/btree.cpp
  src/catalog.cpp
  src/common.cpp
  src/engine.cpp
  src/fault.cpp
  src/file.cpp
  src/page.cpp
  src/schema.cpp
  src/sql_exec.cpp
  src/sql_parser.cpp
  src/wal.cpp
)
target_include_directories(dbengine_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(dbengine_core PUBLIC cxx_std_23)
add_library(db_engine::core ALIAS dbengine_core)

add_executable(db_engine src/main.cpp)
target_link_libraries(db_engine PRIVATE dbengine_core)
if(WIN32)
  target_link_libraries(db_engine PRIVATE psapi)
endif()
target_compile_definitions(db_engine PRIVATE DB_ENGINE_VERSION="${PROJECT_VERSION}")

if(BUILD_TESTING)
  add_executable(dbengine_tests
    tests/test_main.cpp
    tests/test_bplus.cpp
    tests/test_buffer_pool.cpp
    tests/test_engine.cpp
    tests/test_page.cpp
    tests/test_parser.cpp
    tests/test_wal.cpp
  )
  target_link_libraries(dbengine_tests PRIVATE dbengine_core)
  add_test(NAME unit COMMAND dbengine_tests)
  set_tests_properties(unit PROPERTIES LABELS "unit")

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME recovery
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/recovery/recovery_test.py
        --exe $<TARGET_FILE:db_engine>
        --workdir ${CMAKE_CURRENT_BINARY_DIR}/recovery-work)
    set_tests_properties(recovery PROPERTIES LABELS "recovery" TIMEOUT 900)
  endif()

  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES)
    add_test(NAME bench_100k
      COMMAND db_engine --bench --rows 100000 --queries 200
              --buffer-pages 4096)
    set_tests_properties(bench_100k PROPERTIES LABELS "bench" TIMEOUT 600)
  endif()
endif()

install(TARGETS db_engine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
