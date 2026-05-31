set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/native-gcc16.toolchain.cmake")

# -march/-mtune 已在 toolchain 中设置，不在此处重复（避免传给 nasm 等工具）
set(VCPKG_C_FLAGS "-O3 -flto -ffunction-sections -fdata-sections")
set(VCPKG_CXX_FLAGS "-O3 -flto -ffunction-sections -fdata-sections")
set(VCPKG_LINKER_FLAGS "-flto -Wl,--gc-sections")
