# Native compilation toolchain (host machine)
# Uses the system default g++/gcc which is GCC 16 on this machine.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)

# 使用 CMAKE_<LANG>_FLAGS 而非 add_compile_options，避免传给 nasm 等汇编工具
set(CMAKE_C_FLAGS_INIT "-march=native -mtune=native")
set(CMAKE_CXX_FLAGS_INIT "-march=native -mtune=native")
