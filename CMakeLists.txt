cmake_minimum_required(VERSION 3.22)

project(NUEDC LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Linkage mode ─────────────────────────────────────────────────────
# ON  = everything statically linked, single self-contained binary.
# OFF = vcpkg libs dynamic (.so), faster incremental linking; shared
#       libs are automatically bundled at install time.
option(NUEDC_STATIC_LINKING "Static-link all dependencies"
    "$ENV{NUEDC_STATIC_LINKING}")

# -freflection 仅 GCC 16+ 支持
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-freflection HAS_FREFLECTION)
if(HAS_FREFLECTION)
    add_compile_options(-freflection)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

if(NUEDC_STATIC_LINKING)
    add_link_options(-static-libstdc++ -static-libgcc)
endif()

# Release 专属优化
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -fno-semantic-interposition)
    add_link_options(-s)

    # LTO: whole-program optimization
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        # GCC: single partition for maximum cross-TU inlining
        # Clang: -flto (full) already does this; -flto=thin is per-triplet
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_link_options(-flto-partition=one)
        endif()
    endif()

    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
else()
    add_compile_options(-O0 -g)
endif()

find_package(Eigen3 CONFIG REQUIRED)
find_package(OpenCV REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(ryml CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
find_package(hfsm2 CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(PkgConfig QUIET)
if(NOT LIBUSB_FOUND)
    find_library(LIBUSB_LIBRARIES usb-1.0)
    find_path(LIBUSB_INCLUDE_DIRS libusb.h PATH_SUFFIXES libusb-1.0)
    if(LIBUSB_LIBRARIES AND LIBUSB_INCLUDE_DIRS)
        set(LIBUSB_FOUND TRUE)
    endif()
endif()

# ── FlatBuffers code generation ──────────────────────────────────────────
set(FBS_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${FBS_GENERATED_DIR})

set(FBS_SCHEMAS
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/host_to_slave.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/slave_to_host.fbs
)

set(FBS_GENERATED_HEADERS
    ${FBS_GENERATED_DIR}/host_to_slave_generated.h
    ${FBS_GENERATED_DIR}/slave_to_host_generated.h
)

add_custom_command(
    OUTPUT ${FBS_GENERATED_HEADERS}
    COMMAND flatc --cpp --gen-mutable --scoped-enums
            -o ${FBS_GENERATED_DIR}
            ${FBS_SCHEMAS}
    DEPENDS ${FBS_SCHEMAS}
    COMMENT "Generating FlatBuffers headers"
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_executable(main
    src/main.cpp
    ${FBS_GENERATED_HEADERS}
)

target_include_directories(main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${FBS_GENERATED_DIR}
)

target_link_libraries(main PRIVATE
    Eigen3::Eigen
    ${OpenCV_LIBS}
    spdlog::spdlog_header_only
    ryml::ryml
    flatbuffers::flatbuffers
    hfsm2::hfsm2
    magic_enum::magic_enum
    $<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>
)

if(LIBUSB_FOUND)
    target_include_directories(main PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_link_libraries(main PRIVATE ${LIBUSB_LIBRARIES})
    target_compile_definitions(main PRIVATE NUEDC_HAS_LIBUSB=1)
endif()

# RPATH: 二本体在 bin/，库在 lib/，用相对路径找
set_target_properties(main PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)

# ====================== install ======================
include(GNUInstallDirs)

install(TARGETS main RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/config DESTINATION .)

# ── Dynamic linkage: bundle vcpkg shared libraries ───────────────────
if(NOT NUEDC_STATIC_LINKING)
    set(_vcpkg_lib "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib")
    install(DIRECTORY "${_vcpkg_lib}/"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        FILES_MATCHING
        PATTERN "*.so" PATTERN "*.so.*"
        PATTERN "pkgconfig" EXCLUDE
        PATTERN "cmake" EXCLUDE
    )
endif()

# ── Cross-compile aarch64: bundle runtime libs + wrapper ────────────
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    # Find libstdc++ / libgcc from the cross-compiler's search path
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.so.6
        OUTPUT_VARIABLE _libstdcxx OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libgcc_s.so.1
        OUTPUT_VARIABLE _libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)

    set(_runtime_libs
        # glibc core
        "ld-linux-aarch64.so.1"
        "libc.so.6"
        "libm.so.6"
        "libpthread.so.0"
        "libdl.so.2"
        "librt.so.1"
        "libresolv.so.2"
        "libnss_files.so.2"
    )

    # Add C++ runtime if found
    if(EXISTS "${_libstdcxx}")
        list(APPEND _runtime_libs "${_libstdcxx}")
    endif()
    if(EXISTS "${_libgcc}")
        list(APPEND _runtime_libs "${_libgcc}")
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

# ====================== 打印配置信息 ======================
message(STATUS "=================== Build Configuration ===================")
message(STATUS "C++ Standard          : ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler              : ${CMAKE_CXX_COMPILER}")
message(STATUS "Sysroot               : ${CMAKE_SYSROOT}")
if(HAS_FREFLECTION)
    message(STATUS "Reflection enabled    : Yes (-freflection)")
else()
    message(STATUS "Reflection enabled    : No")
endif()
message(STATUS "Build Type            : ${CMAKE_BUILD_TYPE}")
message(STATUS "Static linking        : ${NUEDC_STATIC_LINKING}")
message(STATUS "========================================================")
