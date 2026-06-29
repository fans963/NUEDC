# Native static triplet — self-contained binary, slower link (cross-module LTO).
# For fast dev iteration, use native-gcc16 (dynamic) instead.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/native-gcc16.toolchain.cmake")

set(VCPKG_C_FLAGS "-O3 -flto=auto -ffat-lto-objects -ffunction-sections -fdata-sections")
set(VCPKG_CXX_FLAGS "-O3 -flto=auto -ffat-lto-objects -ffunction-sections -fdata-sections")
set(VCPKG_LINKER_FLAGS "-flto=auto -Wl,--gc-sections")
