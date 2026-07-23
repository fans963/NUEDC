cmake_minimum_required(VERSION 3.22)

project(NUEDC LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ─────────────────────────────────────────────────────────
option(NUEDC_STATIC_LINKING "Static-link all dependencies" "$ENV{NUEDC_STATIC_LINKING}")
option(NUEDC_ENABLE_SANITIZERS "Enable ASan/UBSan for debug builds" OFF)
option(NUEDC_RUST_ONLY "Build only the Rust port (skip C++ and vcpkg)" OFF)
option(NUEDC_BUILD_RUST "Build Rust port alongside C++" OFF)

# ── Rust port helper (must be defined before use) ────────────────────
function(_nuedc_add_rust_target)
    find_program(CARGO cargo REQUIRED)
    set(RUST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust")

    set(RUST_CARGO_FLAGS --release)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        list(APPEND RUST_CARGO_FLAGS --target aarch64-unknown-linux-gnu)
    endif()

    add_custom_target(nuedc_rust ALL
        COMMAND ${CARGO} build ${RUST_CARGO_FLAGS}
        WORKING_DIRECTORY "${RUST_DIR}"
        COMMENT "Building Rust port..."
        USES_TERMINAL
    )

    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(_rust_out "${RUST_DIR}/target/aarch64-unknown-linux-gnu/release/main")
    else()
        set(_rust_out "${RUST_DIR}/target/release/main")
    endif()

    install(PROGRAMS "${_rust_out}"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME main
    )

    # ── aarch64: bundle runtime libs + run.sh (same as C++) ──────────
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(_runtime_libs "")
        foreach(_lib ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libgcc_s.so.1)
            execute_process(
                COMMAND ${CMAKE_C_COMPILER} -print-file-name=${_lib}
                OUTPUT_VARIABLE _found OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(EXISTS "${_found}")
                list(APPEND _runtime_libs "${_found}")
            endif()
        endforeach()

        install(CODE "
            set(_dst \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}\")
            file(MAKE_DIRECTORY \${_dst})
            foreach(_src ${_runtime_libs})
                if(EXISTS \"\${_src}\")
                    execute_process(COMMAND \${CMAKE_COMMAND} -E copy
                        \"\${_src}\" \"\${_dst}/\")
                endif()
            endforeach()
        " COMPONENT runtime)

        install(CODE "
            file(WRITE \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/run.sh\"
\"#!/bin/sh
DIR=\\\$(dirname \\\$(readlink -f \\\$0))
exec \\\$DIR/../lib/ld-linux-aarch64.so.1 --library-path \\\$DIR/../lib \\\$DIR/main \\\$@
\")
            execute_process(COMMAND chmod +x \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/run.sh\")
        " COMPONENT runtime)
    endif()

    message(STATUS "  Rust port         : enabled")
endfunction()

# ── Rust-only mode: skip C++ entirely ───────────────────────────────
if(NUEDC_RUST_ONLY)
    include(GNUInstallDirs)
    _nuedc_add_rust_target()
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/config DESTINATION .)
    return()
endif()

# ── Compiler flags ──────────────────────────────────────────────────
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-freflection HAS_FREFLECTION)
if(HAS_FREFLECTION)
    add_compile_options(-freflection)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

if(NUEDC_STATIC_LINKING)
    add_link_options(-static-libstdc++ -static-libgcc)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -fno-semantic-interposition)
    add_link_options(-s)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_link_options(-flto-partition=one)
    endif()
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
else()
    add_compile_options(-O0 -g)
    if(NUEDC_ENABLE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# ── Dependencies ────────────────────────────────────────────────────
find_package(Eigen3 CONFIG REQUIRED)
find_package(OpenCV REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(ryml CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
find_package(hfsm2 CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(foxglove-sdk CONFIG QUIET)
find_package(cserialport CONFIG REQUIRED)
find_package(re2 CONFIG REQUIRED)

# ── FlatBuffers code generation ─────────────────────────────────────
set(FBS_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${FBS_GENERATED_DIR})

file(GLOB FBS_SCHEMAS ${CMAKE_CURRENT_SOURCE_DIR}/schemas/*.fbs)
set(FBS_GENERATED_HEADERS "")
foreach(_fbs ${FBS_SCHEMAS})
    get_filename_component(_name ${_fbs} NAME_WE)
    list(APPEND FBS_GENERATED_HEADERS ${FBS_GENERATED_DIR}/${_name}_generated.h)
endforeach()

add_custom_command(
    OUTPUT ${FBS_GENERATED_HEADERS}
    COMMAND flatc --cpp --gen-mutable --scoped-enums
            -o ${FBS_GENERATED_DIR} ${FBS_SCHEMAS}
    DEPENDS ${FBS_SCHEMAS}
    COMMENT "Generating FlatBuffers headers"
)

# ── Collect sources ─────────────────────────────────────────────────
file(GLOB_RECURSE NUEDC_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_executable(main ${NUEDC_SOURCES} ${FBS_GENERATED_HEADERS})

target_include_directories(main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${FBS_GENERATED_DIR}
)

# ── Link dependencies ──────────────────────────────────────────────
target_link_libraries(main PRIVATE
    Eigen3::Eigen
    ${OpenCV_LIBS}
    spdlog::spdlog_header_only
    ryml::ryml
    flatbuffers::flatbuffers
    hfsm2::hfsm2
    $<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>
)

target_include_directories(main PRIVATE ${CSerialPort_INCLUDE_DIR})
target_link_libraries(main PRIVATE ${CSerialPort_LIBRARY} re2::re2)

# Foxglove WebSocket bridge
if(TARGET foxglove-sdk::foxglove_cpp_static)
    target_link_libraries(main PRIVATE foxglove-sdk::foxglove_cpp_static)
else()
    target_link_libraries(main PRIVATE foxglove-sdk::foxglove_cpp_shared)
endif()

# ── Install ─────────────────────────────────────────────────────────
include(GNUInstallDirs)

set_target_properties(main PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib"
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
)

if(NUEDC_BUILD_RUST)
    _nuedc_add_rust_target()
endif()

install(TARGETS main RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/config DESTINATION .)

# Bundle shared libraries (release + debug)
if(NOT NUEDC_STATIC_LINKING)
    foreach(_config_type lib debug/lib)
        set(_vcpkg_lib "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/${_config_type}")
        if(EXISTS "${_vcpkg_lib}")
            install(DIRECTORY "${_vcpkg_lib}/"
                DESTINATION ${CMAKE_INSTALL_LIBDIR}
                FILES_MATCHING
                PATTERN "*.so" PATTERN "*.so.*"
                PATTERN "pkgconfig" EXCLUDE
                PATTERN "cmake" EXCLUDE
            )
        endif()
    endforeach()
endif()

# ── Cross-compile aarch64: bundle runtime libs + wrapper ────────────
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.so.6
        OUTPUT_VARIABLE _libstdcxx OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libgcc_s.so.1
        OUTPUT_VARIABLE _libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)

    set(_runtime_libs "")
    foreach(_lib ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libpthread.so.0
                 libdl.so.2 librt.so.1 libresolv.so.2 libnss_files.so.2)
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${_lib}
            OUTPUT_VARIABLE _found OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(EXISTS "${_found}")
            list(APPEND _runtime_libs "${_found}")
        endif()
    endforeach()

    if(EXISTS "${_libstdcxx}")
        list(APPEND _runtime_libs "${_libstdcxx}")
    endif()
    if(EXISTS "${_libgcc}")
        list(APPEND _runtime_libs "${_libgcc}")
    endif()

    # libatomic (required by foxglove-sdk / Rust)
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libatomic.so.1
        OUTPUT_VARIABLE _libatomic OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(EXISTS "${_libatomic}")
        list(APPEND _runtime_libs "${_libatomic}")
    endif()

    install(CODE "
        set(_dst \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}\")
        file(MAKE_DIRECTORY \${_dst})
        foreach(_src ${_runtime_libs})
            if(EXISTS \"\${_src}\")
                execute_process(COMMAND \${CMAKE_COMMAND} -E copy
                    \"\${_src}\" \"\${_dst}/\")
            endif()
        endforeach()
    " COMPONENT runtime)

    install(CODE "
        file(WRITE \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/run.sh\"
\"#!/bin/sh
DIR=\\\$(dirname \\\$(readlink -f \\\$0))
exec \\\$DIR/../lib/ld-linux-aarch64.so.1 --library-path \\\$DIR/../lib \\\$DIR/main \\\$@
\")
        execute_process(COMMAND chmod +x \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/run.sh\")
    " COMPONENT runtime)
endif()

# ── Configuration summary ──────────────────────────────────────────
message(STATUS "=================== NUEDC Build ===================")
message(STATUS "  C++ Standard     : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler         : ${CMAKE_CXX_COMPILER}")
message(STATUS "  Build Type       : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Static linking   : ${NUEDC_STATIC_LINKING}")
message(STATUS "  Sanitizers       : ${NUEDC_ENABLE_SANITIZERS}")
message(STATUS "  Reflection       : ${HAS_FREFLECTION}")
message(STATUS "  OpenCV           : ${OpenCV_VERSION} (${OpenCV_LIBS})")
message(STATUS "  Sysroot          : ${CMAKE_SYSROOT}")
message(STATUS "====================================================")
