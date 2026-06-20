set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/aarch64-gcc16.toolchain.cmake")

# 全志 A733: 2x Cortex-A76 + 6x Cortex-A55 (ARMv8.2-A)
set(VCPKG_C_FLAGS "-O3 -flto -march=armv8.2-a+crypto+fp16+dotprod+rdma+lse+rcpc -mtune=cortex-a76")
set(VCPKG_CXX_FLAGS "-O3 -flto -march=armv8.2-a+crypto+fp16+dotprod+rdma+lse+rcpc -mtune=cortex-a76")
set(VCPKG_LINKER_FLAGS "-flto")

if("$ENV{NUEDC_USE_SYSROOT}" STREQUAL "ON")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
        "-DCMAKE_SYSROOT=${CMAKE_CURRENT_LIST_DIR}/../sysroot"
    )
endif()
