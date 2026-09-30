cmake_minimum_required(VERSION 3.25)
project(cmc_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_library(LZ4_LIBRARY lz4 REQUIRED)
find_path(LZ4_INCLUDE_DIR lz4.h REQUIRED)

# Tables generated from the vanilla data reports in data/.
set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
set(GEN_OUTPUTS
  ${GEN_DIR}/packet_ids.hpp
  ${GEN_DIR}/blocks.hpp ${GEN_DIR}/blocks.cpp
  ${GEN_DIR}/registries.hpp ${GEN_DIR}/registries.cpp
  ${GEN_DIR}/behaviour.hpp ${GEN_DIR}/behaviour.cpp)
file(GLOB DATA_FILES ${CMAKE_SOURCE_DIR}/data/*.json)
add_custom_command(
  OUTPUT ${GEN_OUTPUTS}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_data.py ${GEN_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_data.py ${DATA_FILES}
  COMMENT "Generating data tables")

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(cmc-core STATIC ${SOURCES} ${GEN_DIR}/blocks.cpp ${GEN_DIR}/registries.cpp ${GEN_DIR}/behaviour.cpp)
target_include_directories(cmc-core PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR})
target_link_libraries(cmc-core PUBLIC ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto ${LZ4_LIBRARY})
target_include_directories(cmc-core PUBLIC ${LZ4_INCLUDE_DIR})
target_compile_options(cmc-core PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(cmc-server ${CMAKE_SOURCE_DIR}/src/main.cpp)
target_link_libraries(cmc-server PRIVATE cmc-core)
target_compile_options(cmc-server PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Developer tool: prints item component hashes for a saved player file (tools/test_components.sh).
add_executable(cmc-codec-check EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tools/codec_check.cpp)
target_link_libraries(cmc-codec-check PRIVATE cmc-core)

# Developer tool: places a world-gen feature with a seed and prints the blocks (tools/feature_check.sh).
add_executable(cmc-feature-check EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tools/feature_check.cpp)
target_link_libraries(cmc-feature-check PRIVATE cmc-core)
