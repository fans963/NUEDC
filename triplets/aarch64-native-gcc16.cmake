set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/aarch64-gcc16.toolchain.cmake")

set(VCPKG_C_FLAGS "-O3 -flto -march=armv8-a -mtune=cortex-a72 -ffunction-sections -fdata-sections")
set(VCPKG_CXX_FLAGS "-O3 -flto -march=armv8-a -mtune=cortex-a72 -ffunction-sections -fdata-sections")
set(VCPKG_LINKER_FLAGS "-flto -Wl,--gc-sections")

# sysroot: 由主项目通过环境变量 NUEDC_USE_SYSROOT=ON 启用
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DMI_NO_OPT_ARCH=ON"  # mimalloc: disable armv8.1-a LSE, stick to armv8-a
)

if("$ENV{NUEDC_USE_SYSROOT}" STREQUAL "ON")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
        "-DCMAKE_SYSROOT=${CMAKE_CURRENT_LIST_DIR}/../sysroot"
    )
endif()
