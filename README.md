# NUEDC

全国大学生电子设计竞赛嵌入式开发项目，基于 aarch64 交叉编译。

## 技术栈

- **C++26** — 使用 GCC 16 的 `-freflection` 反射特性
- **vcpkg** — C++ 包管理
- **Docker** — 开发容器

## 项目结构

```
NUEDC/
├── .devcontainer/        # VS Code 开发容器配置
├── src/                  # 源码
│   └── main.cpp
├── toolchains/           # CMake 交叉编译工具链
│   └── aarch64-gcc16.toolchain.cmake
├── triplets/             # vcpkg 自定义 triplet
│   └── aarch64-native-gcc16.cmake
├── CMakeLists.txt
├── vcpkg.json
└── Dockerfile            # 开发容器镜像
```

## 快速开始

### 1. 打开开发容器

用 VS Code 打开项目，选择「在容器中重新打开」。容器基于 Debian Sid，预装了 GCC 16 交叉编译器和 vcpkg。

### 2. 构建

```bash
mkdir -p build && cd build
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=/workspace/toolchains/aarch64-gcc16.toolchain.cmake \
  -DVCPKG_TARGET_TRIPLET=aarch64-native-gcc16 \
  -DVCPKG_OVERLAY_TRIPLETS=/workspace/triplets \
  -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 3. 打包部署

```bash
cmake --install . --prefix ./install
```

### 4. 测试（qemu 模拟）

```bash
qemu-aarch64 ./install/lib/ld-linux-aarch64.so.1 --library-path ./install/lib ./install/bin/main
```


打包产物结构：

```
install/
├── bin/
│   ├── main              # 可执行文件
│   └── run.sh            # 启动脚本（板子上用）
└── lib/
    ├── ld-linux-aarch64.so.1   # 自带的动态加载器
    ├── libc.so.6               # 自带的 glibc
    ├── libstdc++.so.6          # GCC 16 运行时
    ├── libopencv_*.so          # OpenCV 库
    └── ...
```

部署到板子：

```bash
scp -r install/* board:/opt/nuedc/
ssh board /opt/nuedc/bin/run.sh
```

## 关于运行时库

本项目将所有运行时库（glibc、libstdc++、libgcc、OpenCV）打包到了 install 目录中，**不依赖板子系统的库版本**。

`run.sh` 会使用自带的 `ld-linux-aarch64.so.1` 作为动态加载器，从 `../lib/` 加载所有依赖库。这样即使板子系统较旧（如 Debian Bullseye），也能正常运行。

glibc 与硬件架构相关（本项目为 aarch64），与内核版本的关系是向后兼容 — 新 glibc 可以在老内核上运行（要求内核 >= 3.2）。

## 可选：使用 Sysroot

默认不使用 sysroot。如果有需要，可以在 toolchain 中启用：

```bash
cmake .. -DUSE_SYSROOT=ON ...
```

启用后会使用 `sysroot/` 目录作为目标系统的根文件系统，适用于需要引用板子上预装库的场景。

## C++26 反射注意事项

- GCC 16 的反射语法使用 `^^`（双 caret）而非 `^` 反射类型
- splice 语法为 `[:meta_info:]`
- 头文件为 `<meta>`，API 在 `std::meta::` 命名空间下
- `nonstatic_data_members_of()` 需要传入 `access_context` 参数（如 `std::meta::access_context::unprivileged()`）
- 反射返回的 `std::vector` 不能在编译期遍历（`template for` + `constexpr` 会因 heap 分配报错）
- clangd 不支持 `-freflection`，已在 `.clangd` 中过滤该参数
