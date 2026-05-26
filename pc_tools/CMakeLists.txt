cmake_minimum_required(VERSION 3.16)
project(he_tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to local SEAL clone + local minimal build from this workspace.
set(SEAL_SOURCE_DIR_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/../SEAL")
set(SEAL_BUILD_DIR_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/../SEAL/build-min")

set(SEAL_SOURCE_DIR "${SEAL_SOURCE_DIR_DEFAULT}" CACHE PATH "Path to SEAL source tree")
set(SEAL_BUILD_DIR "${SEAL_BUILD_DIR_DEFAULT}" CACHE PATH "Path to SEAL build directory")

add_executable(
    he_tool
    src/he_tool.cpp
    src/he_bundle.cpp
    ../esp32_client/src/he_allocator.cpp
    ../esp32_client/src/he_embedded_package.cpp
    ../esp32_client/src/mini_ckks_client.cpp)

target_include_directories(
    he_tool
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/include
        ${SEAL_SOURCE_DIR}/native/src
        ${SEAL_BUILD_DIR}/native/src
        ${CMAKE_CURRENT_LIST_DIR}/../esp32_client/src)

find_library(SEAL_LIB NAMES seal-4.3 seal PATHS ${SEAL_BUILD_DIR}/lib NO_DEFAULT_PATH)
if(NOT SEAL_LIB)
    message(FATAL_ERROR "Could not find SEAL static library in ${SEAL_BUILD_DIR}/lib")
endif()

target_link_libraries(he_tool PRIVATE ${SEAL_LIB})
