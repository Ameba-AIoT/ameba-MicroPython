<div align="center">

<img src="upython-with-micro.png" alt="MicroPython for Ameba-RTOS" width="480">

# MicroPython for Ameba-RTOS

**直接在 Realtek Ameba 芯片上用 Python 编写 Wi-Fi、网络、文件系统和外设应用。**

[![MicroPython](https://img.shields.io/badge/MicroPython-v1.28-blue.svg)](https://micropython.org)
[![SoC](https://img.shields.io/badge/SoC-AmebaDplus%20RTL8721Dx-green.svg)](#-支持的硬件)
[![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS-orange.svg)](https://www.freertos.org)
[![License](https://img.shields.io/badge/License-MIT-lightgrey.svg)](LICENSE)
[![Status](https://img.shields.io/badge/status-active%20development-yellow.svg)](#-路线图)

[English](README.md) · [中文](README_CN.md) · [路线图](#-路线图) · [SDK 使用指南](https://aiot.realmcu.com/zh/latest/rtos/sdk/index.html)

</div>

本项目是将 [MicroPython](https://micropython.org) 移植到
[Realtek Ameba-RTOS](https://github.com/Ameba-AIoT) SDK。移植代码位于
`ports/ameba-rtos-m/`，让你能直接在 Ameba 芯片上用 Python 编写 Wi-Fi、网络、
文件系统和外设应用——通过串口连接即可获得交互式 REPL，无需重新编译固件。

主要目标平台是 **AmebaDplus (RTL8721Dx)**。**AmebaGreen2
(RTL8721F/RTL8711F)** 也在积极移植中（见 EV8711FLM 板），后续将通过切换
`soc_info.json` 支持 AmebaLite 和 AmebaSmart。

本移植处于积极开发阶段，部分 `machine` 外设模块仍在开发中。当前状态见
[路线图](#-路线图)。

## 🚀 快速上手

MicroPython API 参考和通用用法说明，请见
[在线文档](https://docs.micropython.org/)。

### 前置条件

- Linux 主机（原生或 WSL2）。
- `git` 与 Python 3.8+。
- `source ameba-rtos/env.sh` 会在首次使用时自动准备交叉工具链和 Python
  虚拟环境——它从 GitHub（或阿里云镜像）下载 prebuilts，所以首次运行需联网，
  之后会复用。

### 获取源码

```bash
git clone <repo-url> MicroPython
cd MicroPython
```

### 编译（推荐）

根目录 `Makefile` 委托给 port 级 Makefile，由后者负责 board 选择、工具链
环境和固件打包：

```bash
make                                    # 增量编译（默认：BOARD=PKE8721DAF）
make BOARD=PKE8721DAF                   # 显式指定 board
make pristine                           # 全量（pristine）编译
make clean                              # 清理
make deploy PORT=/dev/ttyUSB0           # 仅烧录（跳过编译）
make build deploy PORT=/dev/ttyUSB0     # 先编译再烧录
```

首次编译前需手动初始化子模块（见下方）。

### 编译（手动）

如果你想自己逐步执行：

```bash
# 一次性：初始化编译所需的子模块
git submodule update --init micropython ameba-rtos
cd micropython
git submodule update --init lib/berkeley-db-1.xx lib/micropython-lib
cd ..

# 每个 shell 会话：初始化工具链，然后编译
cd ports/ameba-rtos-m
make BOARD=PKE8721DAF           # 增量编译
make BOARD=PKE8721DAF pristine  # 全量编译
```

两种方式下，编译成功时你都应看到 **`Build done`**。`mpy-cross` 字节码编译器
会在首次运行时自动构建。

> [!TIP]
> 工具链配置、编译与烧录的详细说明，见
> [Ameba-RTOS SDK 使用指南](https://aiot.realmcu.com/zh/latest/rtos/sdk/index.html)。

### 烧录与运行

编译产物（固件镜像）位于 `ports/ameba-rtos-m/build_PKE8721DAF/`（`boot.bin`、
`app.bin`、`firmware.bin`）。最便捷的方式是 `make deploy`，它跳过编译直接烧录，
适合重复烧录场景（`PORT` 必传；遇到偶发烧录失败会自动重试，`BAUD=` 可覆盖波特率）：

```bash
make deploy PORT=/dev/ttyUSB0                        # 仅烧录（跳过编译）
make build deploy PORT=/dev/ttyUSB0                  # 先编译再烧录
make build deploy PORT=/dev/ttyUSB0 BAUD=115200
make BOARD=PKE8721DAF deploy PORT=/dev/ttyUSB0       # 显式指定 board
```

也可以对已有的编译产物手动烧录（`ameba.py flash -h` 查看 `-b <波特率>`、
`--chip-erase` 等选项）：

```bash
cd ports/ameba-rtos-m
python ../../ameba-rtos/ameba.py flash -p /dev/ttyUSB0 -dev RTL8721Dx
```

#### 烧录预编译的 release 固件（不需要 SDK）

- **[AmebaFlash](https://aiot.realmcu.com/download/latest/Tools/AmebaFlash.zip)**
  （命令行，跨平台）
- **[Image Tool](https://aiot.realmcu.com/download/latest/Tools/ImageTool.zip)**
  （图形界面，Windows）

烧录 release 固件时，起始地址填 `0x08000000`。

然后通过 LOGUART 用任意串口终端连接：

```bash
picocom -b 115200 /dev/ttyACM0
```

> [!IMPORTANT]
> LOGUART 默认运行在 **115200 波特率**，请把你的终端设为同样的波特率。
> 若要用其他速率（例如 `1500000`），在 `src/mpconfigport.h` 中修改
> `MICROPY_HW_LOGUART_BAUDRATE` 后重新编译。

连接后即可使用 REPL：

```python
>>> import network
>>> wlan = network.WLAN(network.STA_IF)
>>> wlan.active(True)
>>> wlan.connect("your-ssid", "your-password")
>>> wlan.isconnected()
True
>>> wlan.ifconfig()
('192.168.1.123', '255.255.255.0', '192.168.1.1', '8.8.8.8')
```

## ✨ 特性

- 通过 LOGUART 串口的完整 MicroPython REPL，支持软复位（soft reset）和粘贴
  模式（paste mode）
- Wi-Fi 网络：`network.WLAN` 的 STA 和 AP 模式，含 scan、connect、ifconfig
  及状态查询方法
- 基于 lwIP 的 BSD sockets：TCP/UDP，支持阻塞、非阻塞、超时及 SSL
- Flash 文件系统：`ameba.Flash` 块设备配合 littlefs（LFS2）VFS，`boot.py` 与
  `main.py` 在掉电后持久保存
- 冻结模块（Frozen modules）：`bundle-networking`、`umqtt`、`dht`、`neopixel`
  等已预编译进固件
- 多线程：由 FreeRTOS 任务支撑的 `_thread` 模块
- `machine` 外设 API：`Pin`、`UART`（含 IRQ / sendbreak）、`SPI`、`SoftSPI`、
  `I2C`、`SoftI2C`、`I2CTarget`、`ADC`、`PWM`、`RTC`、`WDT`、`Timer`、`I2S`、
  `bitstream`（WS2812/NeoPixel，基于 LEDC 硬件 + DMA 加速）、`lightsleep`、
  `deepsleep`、`wake_reason`、`bootloader`
- `os.dupterm` 支持 WebREPL 及多控制台 REPL
- `hashlib`（SHA256/SHA1/MD5）、`cryptolib`（AES）、`onewire`、`dht`
- OTA 固件升级：`ameba.Partition` / `ameba.OTA`

## 🔌 支持的硬件

| SoC                              | 状态     |
|----------------------------------|----------|
| AmebaDplus (RTL8721Dx)           | 进行中   |
| AmebaGreen2 (RTL8721F / RTL8711F) | 移植中  |
| AmebaLite (RTL8720E / RTL8710E)  | 计划中   |
| AmebaSmart (RTL8730E)            | 计划中   |

## 🏗️ 架构

本移植作为 `ameba_add_internal_library(micropython)` 组件，在 ameba-rtos 的
CMake 框架内编译。入口点是 `mp_main.c` 中的 `app_example()`，它创建一个运行
`mp_main()` 的 FreeRTOS 任务。

启动流程：

```text
app_example() --> FreeRTOS task --> mp_main() --> _boot.py --> boot.py --> main.py --> REPL
```

软复位通过 `goto soft_reset` 重新初始化 GC 堆和解释器，而不重启 RTOS 任务。

仓库结构：

```text
MicroPython/
├── ports/
│   └── ameba-rtos-m/          # Port 目录（主要工作区）
│       ├── src/               # Port C 源码
│       │   ├── mp_main.c      #   入口：app_example() -> mp_main() -> REPL
│       │   ├── mphalport.c    #   UART HAL（stdin_ringbuf / LOGUART）
│       │   ├── mpthreadport.c #   _thread -> FreeRTOS task
│       │   ├── network_wlan.c #   network.WLAN（Ameba Wi-Fi API）
│       │   ├── modsocket.c    #   BSD socket（lwIP）
│       │   ├── ameba_flash.c  #   ameba.Flash（VFS 块设备）
│       │   ├── modmachine.c   #   machine 模块
│       │   └── mpconfigport.h #   特性开关 / 堆大小
│       ├── boards/manifest.py #   冻结模块列表
│       └── modules/_boot.py   #   启动脚本（冻结）
├── micropython/               # MicroPython 上游（子模块，只读）
└── ameba-rtos/                # Realtek Ameba-RTOS SDK（子模块，只读）
    ├── ameba.py               #   统一构建 CLI 入口
    └── env.sh                 #   工具链环境初始化
```

关键 port 文件：

| 文件               | 作用                                      |
|--------------------|-------------------------------------------|
| `mpconfigport.h`   | 所有 MicroPython 特性开关；堆大小         |
| `mphalport.c`      | 经 `stdin_ringbuf`、`LOGUART` 的 UART I/O |
| `mpthreadport.c`   | 经 FreeRTOS 任务的线程支持                |
| `modameba.c`       | `ameba` Python 模块（Flash 访问）         |
| `modsocket.c`      | BSD socket API                            |
| `network_wlan.c`   | `network.WLAN` STA/AP 实现（Ameba Wi-Fi API） |

## 🗺️ 路线图

下表按**实现先后顺序**排列（`Phase` 编号是稳定标识，不代表顺序）。

| Phase | 内容                                                           | PKE8721DAF               | EV8711FLM                |
|:-----:|----------------------------------------------------------------|:------------------------:|:------------------------:|
| 0     | 代码审计（API 残留扫描、QSTR 完整性）                          | 完成                     | 完成                     |
| 1     | `network` — Wi-Fi STA / AP / scan                              | 完成                     | 完成                     |
| 1.5   | Flash FS 布局修正（`ameba.Flash` + VFS）                       | 完成                     | 完成                     |
| 2     | `machine` — `unique_id()` / `reset_cause()`                    | 完成                     | 完成                     |
| 3     | `machine.Pin`（数字读写 + IRQ）                                | 完成                     | 完成                     |
| 4     | `machine.UART`（含 IRQ / sendbreak）                           | 完成                     | 完成                     |
| 5     | `machine.SPI` / `SoftSPI`                                      | 完成                     | 完成                     |
| 6     | `machine.I2C` / `SoftI2C`                                      | 完成                     | 完成                     |
| 7     | `machine.ADC`                                                  | 完成                     | 完成                     |
| 8     | `machine.PWM`                                                  | 完成                     | 完成                     |
| 9     | `machine.Timer`                                                | 完成                     | 完成                     |
| 10    | `machine.RTC`（含 `alarm` / `irq`）                            | 完成                     | 完成                     |
| 11    | `machine.WDT`                                                  | 完成                     | 完成                     |
| 13    | `machine.I2S`                                                  | 完成                     | 完成                     |
| 14    | `machine.SDCard`                                               | 不适用（无 SD 主控制器）  | 完成                     |
| 16    | `ameba.Partition` / OTA                                        | 完成                     | 完成                     |
| 20    | `machine.lightsleep` / `deepsleep` / `wake_reason`             | 完成                     | 完成                     |
| 21    | `time_pulse_us`                                                | 完成                     | 完成                     |
| 22    | `machine.bitstream`（WS2812/NeoPixel），LEDC 硬件 DMA 后端       | 完成                   | 完成                     |
| 27    | `machine.bootloader()`                                         | 完成                     | 完成                     |
| 28    | `os.dupterm` / WebREPL                                         | 完成                     | 完成                     |
| 31    | `machine.I2CTarget`（I2C 从机）                                 | 完成                     | 完成                     |
| 33    | `machine.CAN`                                                  | 不适用（无 CAN 控制器）   | 完成                     |
| 34    | `network.LAN`（Ethernet）                                      | 不适用（无 RMII MAC）     | 完成                     |
| 15    | USB CDC REPL                                                   | 计划中                   | 计划中                   |
| 12    | Bluetooth BLE（GAP / GATT）                                    | 计划中                   | 计划中                   |

*完成* 指实现已合入并在对应板子的硬件上验证通过。
*不适用* 指该 SoC 没有实现这个功能所需的硬件。
