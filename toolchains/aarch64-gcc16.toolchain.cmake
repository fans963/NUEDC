set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Nix cross shell 提供 aarch64-unknown-linux-gnu-*，
# 系统包提供 aarch64-linux-gnu-*，两者都尝试
find_program(_CROSS_GCC
    NAMES aarch64-unknown-linux-gnu-gcc aarch64-linux-gnu-gcc)
find_program(_CROSS_GXX
    NAMES aarch64-unknown-linux-gnu-g++ aarch64-linux-gnu-g++)

if(NOT _CROSS_GCC)
    message(FATAL_ERROR "aarch64 cross compiler not found")
endif()

set(CMAKE_C_COMPILER ${_CROSS_GCC})
set(CMAKE_CXX_COMPILER ${_CROSS_GXX})

find_program(_CROSS_STRIP
    NAMES aarch64-unknown-linux-gnu-strip aarch64-linux-gnu-strip)
if(_CROSS_STRIP)
    set(CMAKE_STRIP ${_CROSS_STRIP})
endif()

# 全志 A733: 2x Cortex-A76 r4p1 + 6x Cortex-A55 r2p0 (DynamIQ big.LITTLE)
# ISA 扩展 (A76 和 A55 的交集, 验证: cat /proc/cpuinfo | grep Features):
#   fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp asimdrdm asimddp lrcpc dcpop
add_compile_options(
    -march=armv8.2-a+crypto+fp16+dotprod+rdma+lse+rcpc
    -mtune=cortex-a76
    -std=gnu++26 -freflection
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)