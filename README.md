# NUEDC

全国大学生电子设计竞赛嵌入式开发项目。主机端（aarch64 SBC）视觉感知 + 决策控制，通过 USB 与下位机（STM32F723）通信。

## 架构

```
┌─────────────────────────────┐       USB Bulk        ┌──────────────────────────────┐
│       NUEDC (主机)           │ ◄──────────────────► │      nuedc_slave (从机)        │
│                             │   FlatBuffers 协议     │                              │
│  组件系统 + HFSM 状态机      │                       │  电机 PID / IMU / 编码器       │
│  OpenCV 视觉处理             │   MotorCommand ──────►│  CAN / UART / SPI / I2C      │
│  ryml YAML 配置              │   ◄────────────────── │  ADC / 透传                   │
│  spdlog 日志                 │   SensorTelemetry     │                              │
└─────────────────────────────┘                       └──────────────────────────────┘
```

## 技术栈

| 类别 | 技术 |
|------|------|
| 语言 | C++26（主机 GCC 16，交叉编译 GCC 15） |
| 构建 | CMake + vcpkg + CMake Presets |
| 组件系统 | 自动注册 + 依赖分析 + 拓扑排序 |
| 序列化 | FlatBuffers（主机↔从机通信） |
| 配置 | ryml（快速 YAML 解析） |
| 日志 | spdlog（共享 logger，组件名前缀） |
| 视觉 | OpenCV 4 |
| 线性代数 | Eigen3 |
| 物理单位 | mp-units |
| 状态机 | HFSM2 |
| 内存 | mimalloc |

## 项目结构

```
NUEDC/
├── include/
│   ├── core/
│   │   ├── component.hpp           # Component 基类 + InputInterface/OutputInterface
│   │   ├── component_registry.hpp  # 自动注册 + 工厂
│   │   └── executor.hpp            # 依赖分析 + 拓扑排序 + 主循环
│   ├── components/                 # 具体组件（Camera, Motor, Heartbeat ...）
│   └── usb/
│       ├── protocol.hpp            # 0x5A | size | FlatBuffer | 0xA5 帧协议
│       └── nuedc_slave.hpp         # USB 封装（libusb）
├── schemas/                        # FlatBuffers schema（与 nuedc_slave 共享）
├── config/
│   └── test.yaml                   # 组件声明 + 参数配置
├── src/
│   └── main.cpp                    # 程序入口
├── toolchains/                     # CMake 工具链
├── triplets/                       # vcpkg triplet
├── CMakePresets.json               # native / cross-aarch64 预设
├── CMakeLists.txt
└── vcpkg.json
```

## 快速开始

### 本机构建（x86_64）

```bash
cmake --preset native
cmake --build build/native
cmake --install build/native --prefix ./build/install
./build/install/bin/main
```

### 交叉编译（aarch64）

```bash
cmake --preset cross-aarch64
cmake --build build/aarch64
cmake --install build/aarch64 --prefix ./build/install
```

部署到板子：

```bash
scp -r build/install/* board:/opt/nuedc/
ssh board /opt/nuedc/bin/run.sh
```

### 调试构建

```bash
cmake --preset native -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native
```

## 组件系统

### 写一个组件

```cpp
// include/components/my_component.hpp
#pragma once
#include "core/component_registry.hpp"

namespace nuedcs::components {

class MyComponent : public core::Component {
public:
    // 通过 InputInterface 绑定其他组件的 Output
    MyComponent() {
        register_input("/status/update_rate", rate_in_);
    }

    void configure(ryml::NodeRef config) override {
        threshold_ = core::node_val<double>(config["threshold"], 0.5);
    }

    bool init() override {
        info("threshold={}", threshold_);
        return true;
    }

    void update() override {
        if (rate_in_.ready() && *rate_in_ > threshold_)
            warn("rate too high: {:.1f}", *rate_in_);
    }

private:
    InputInterface<double> rate_in_;
    double threshold_ = 0.5;
};

} // namespace nuedcs::components

REGISTER_COMPONENT(nuedcs::components, MyComponent);
```

### YAML 配置

```yaml
loop_hz: 1000

components:
  - Camera -> camera1
  - Camera -> camera2
  - Motor -> motor
  - Heartbeat -> heartbeat
  - PredefinedMsgProvider -> status
  - MyComponent -> my_comp

camera1:
  device: /dev/video0
  width: 1280
  height: 720
  fps: 60

my_comp:
  threshold: 0.8
```

- `Type -> instance_name`：同一类型可创建多个实例
- 实例名对应 YAML 中的配置段

### 依赖分析

Executor 自动完成：

1. 收集所有 `OutputInterface` 注册的输出
2. 匹配 `InputInterface` 到对应的输出（按类型 + 路径）
3. Kahn 拓扑排序检测环形依赖
4. DFS 打印依赖树
5. 按拓扑序重排 `update()` 调用顺序

```
── Dependency chain ──────────────────────
  - motor
  - status
  -     camera1
  -     camera2
  -         heartbeat
─────────────────────────────────────────
```

环形依赖会报出成环路径：

```
[error] Circular dependency: status → heartbeat → camera2 → status
```

### 预置组件

`PredefinedMsgProvider` 提供公共输出：

| 输出路径 | 类型 | 含义 |
|----------|------|------|
| `/status/loop_hz` | `double` | 主循环频率 |
| `/status/update_count` | `int64_t` | 总更新计数 |

其他组件通过 `register_input` 绑定即可读取。

## USB 通信（nuedc_slave）

主机通过 USB Bulk 与下位机（STM32F723 + Zephyr）通信，协议为 FlatBuffers 帧：

```
0x5A | u32_le_size | FlatBuffer_body | 0xA5
```

使用方式：

```cpp
#include "usb/nuedc_slave.hpp"

class MyBoard : public nuedc::NuedcSlave {
public:
    using NuedcSlave::NuedcSlave;

    void on_imu(float ax, float ay, float az, float gx, float gy, float gz) override {
        info("IMU: ax={:.2f} ay={:.2f} az={:.2f}", ax, ay, az);
    }
};

int main() {
    MyBoard board;
    board.set_motor_speed(0, 10.0f);
    board.handle_events();
}
```

需要 libusb（vcpkg 自动安装）。交叉编译时通过 `NUEDC_HAS_LIBUSB` 宏条件编译。

## 运行时库打包

交叉编译的 install 包含自带的 glibc + 动态加载器，不依赖板子系统库版本：

```
install/
├── bin/main
├── bin/run.sh
├── config/test.yaml
└── lib/
    ├── ld-linux-aarch64.so.1
    ├── libc.so.6
    └── ...
```

## 注意事项

- clangd 不支持 `-freflection`，已在 `.clangd` 中过滤
- 交叉编译工具链文件名保留 `aarch64-gcc16` 命名（历史原因，实际使用系统 `aarch64-linux-gnu-gcc`）
- `aligned_storage_t` 在 C++23 中 deprecated，不影响功能
