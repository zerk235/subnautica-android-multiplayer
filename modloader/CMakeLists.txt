cmake_minimum_required(VERSION 3.22)
project(snmod LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Presence advertises the exact mod build. The patcher passes a 40-character hash of the
# modloader source tree, including uncommitted edits. Standalone CMake builds fall back to HEAD.
set(SNMOD_GIT_HASH "" CACHE STRING "40-character modloader build identity advertised in LAN presence")
if(NOT SNMOD_GIT_HASH)
    execute_process(
        COMMAND git rev-parse --verify HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE SNMOD_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE SNMOD_GIT_RESULT
    )
    if(NOT SNMOD_GIT_RESULT EQUAL 0)
        set(SNMOD_GIT_HASH "unknown")
        message(WARNING "git metadata unavailable; MOD_VERSION will be 'unknown' and will not "
                        "match hash-versioned peers. Pass -DSNMOD_GIT_HASH=<40-char build ID>.")
    endif()
endif()
if(NOT SNMOD_GIT_HASH STREQUAL "unknown")
    string(LENGTH "${SNMOD_GIT_HASH}" SNMOD_GIT_HASH_LENGTH)
    if(NOT SNMOD_GIT_HASH_LENGTH EQUAL 40 OR NOT SNMOD_GIT_HASH MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "SNMOD_GIT_HASH must be a 40-character hexadecimal build identity")
    endif()
endif()

file(WRITE "${CMAKE_BINARY_DIR}/snmod_build_id.txt" "${SNMOD_GIT_HASH}\n")

set(SHADOWHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/shadowhook/shadowhook/src/main/cpp)

file(GLOB SHADOWHOOK_SRC
    ${SHADOWHOOK_DIR}/*.c
    ${SHADOWHOOK_DIR}/arch/arm64/*.c
    ${SHADOWHOOK_DIR}/arch/arm64/*.S
    ${SHADOWHOOK_DIR}/common/*.c
    ${SHADOWHOOK_DIR}/third_party/xdl/*.c
    ${SHADOWHOOK_DIR}/third_party/bsd/*.c
    ${SHADOWHOOK_DIR}/third_party/lss/*.c
)

add_library(shadowhook_static STATIC ${SHADOWHOOK_SRC})
target_compile_features(shadowhook_static PRIVATE c_std_11)
target_include_directories(shadowhook_static PUBLIC ${SHADOWHOOK_DIR}/include)
target_include_directories(shadowhook_static PRIVATE
    ${SHADOWHOOK_DIR}
    ${SHADOWHOOK_DIR}/arch/arm64
    ${SHADOWHOOK_DIR}/common
    ${SHADOWHOOK_DIR}/third_party/xdl
    ${SHADOWHOOK_DIR}/third_party/bsd
    ${SHADOWHOOK_DIR}/third_party/lss
)
target_link_libraries(shadowhook_static PRIVATE log dl)
target_compile_options(shadowhook_static PRIVATE -Os -ffunction-sections -fdata-sections -w)
target_link_options(shadowhook_static PRIVATE -Wl,-z,max-page-size=16384)

add_library(snmod SHARED
    src/main.cpp
    src/launch_role_bridge.cpp
    src/log.cpp
    src/hooks.cpp
    src/il2cpp_api.cpp
    src/probe.cpp
    src/managed_hook.cpp
    src/net.cpp
    src/world.cpp
)

target_include_directories(snmod PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(snmod PRIVATE MOD_VERSION="${SNMOD_GIT_HASH}")

target_link_libraries(snmod PRIVATE
    shadowhook_static
    log
    dl
    android
)

target_link_options(snmod PRIVATE
    -Wl,-z,max-page-size=16384
    -Wl,--exclude-libs,ALL
)

set_target_properties(snmod PROPERTIES
    OUTPUT_NAME "snmod"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
)
