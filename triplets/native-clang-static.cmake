# Native Clang static triplet — single binary, full LTO
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/native-clang.toolchain.cmake")

set(VCPKG_C_FLAGS "-O3 -flto -ffunction-sections -fdata-sections")
set(VCPKG_CXX_FLAGS "-O3 -flto -ffunction-sections -fdata-sections")
set(VCPKG_LINKER_FLAGS "-flto -Wl,--gc-sections")
