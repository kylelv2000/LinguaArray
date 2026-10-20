cmake_minimum_required(VERSION 3.10)
project(LinguaArray VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies - try multiple methods
find_package(CURL QUIET)
find_package(nlohmann_json QUIET)

# If nlohmann_json package not found, try to include header paths directly
if(NOT nlohmann_json_FOUND)
  message(STATUS "nlohmann_json package not found, trying system include paths")
  include_directories(/usr/include/nlohmann /usr/local/include/nlohmann)
endif()

# Include directories
include_directories(include)

# Configure Python bridge path
set(PARSER_DIR "${CMAKE_SOURCE_DIR}/python_bridge")
configure_file(
    "${CMAKE_SOURCE_DIR}/include/script_paths.h.in"
    "${CMAKE_BINARY_DIR}/include/script_paths.h"
)
include_directories(${CMAKE_BINARY_DIR}/include)

# Source files
set(SOURCES
    src/main.cpp
    src/config.cpp
    src/smt_solver.cpp
    src/llm_client.cpp
    src/engine.cpp
    src/quantifier_helper.cpp
    src/python_parser_adapter.cpp
    src/quantifier_instantiation_adapter.cpp
)

# Create executable
add_executable(linguaarray ${SOURCES})

# Find pthread library (for multithreading)
find_package(Threads REQUIRED)

# Link libraries
target_link_libraries(linguaarray Threads::Threads)
if(nlohmann_json_FOUND)
  target_link_libraries(linguaarray nlohmann_json::nlohmann_json)
endif()

if(CURL_FOUND)
  target_link_libraries(linguaarray ${CURL_LIBRARIES})
  target_include_directories(linguaarray PRIVATE ${CURL_INCLUDE_DIRS})
else()
  # Try to link system's libcurl
  find_library(CURL_LIB NAMES curl libcurl.so.4 PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu)
  if(CURL_LIB)
    target_link_libraries(linguaarray ${CURL_LIB})
  else()
    target_link_libraries(linguaarray curl)
  endif()
endif()

# Compile options
target_compile_options(linguaarray PRIVATE 
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# Static linking option (optional)
option(STATIC_BUILD "Build static executable" OFF)
if(STATIC_BUILD)
    set(CMAKE_EXE_LINKER_FLAGS "-static")
endif()

# Installation configuration
install(TARGETS linguaarray DESTINATION bin)
install(DIRECTORY scripts/ DESTINATION share/linguaarray/scripts
    FILES_MATCHING PATTERN "*.py")

# Testing
enable_testing()
add_test(NAME basic_test COMMAND linguaarray --help) 
