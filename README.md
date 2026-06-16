<div align="center">

<img src="upython-with-micro.png" alt="MicroPython for Ameba-RTOS" width="480">

# MicroPython for Ameba-RTOS

**Write Wi-Fi, networking, filesystem and peripheral applications in Python directly on Realtek Ameba chips.**

[![MicroPython](https://img.shields.io/badge/MicroPython-v1.28-blue.svg)](https://micropython.org)
[![SoC](https://img.shields.io/badge/SoC-AmebaDplus%20RTL8721Dx-green.svg)](#-supported-hardware)
[![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS-orange.svg)](https://www.freertos.org)
[![License](https://img.shields.io/badge/License-MIT-lightgrey.svg)](LICENSE)
[![Status](https://img.shields.io/badge/status-active%20development-yellow.svg)](#-roadmap)

[English](README.md) · [中文](README_CN.md) · [Roadmap](#-roadmap) · [SDK User Guide](https://aiot.realmcu.com/en/latest/rtos/sdk/index.html)

</div>

This is a port of the [MicroPython](https://micropython.org) project to the
[Realtek Ameba-RTOS](https://github.com/Ameba-AIoT) SDK.  The port lives in
`ports/ameba-rtos-m/` and lets you write Wi-Fi, networking, filesystem and
peripheral applications in Python directly on Ameba chips — connect over
serial and get an interactive REPL, no firmware recompilation needed.

The primary target is the **AmebaDplus (RTL8721Dx)**, with AmebaLite,
AmebaGreen2 and AmebaSmart to follow via `soc_info.json` switching.

This is an active development port; some `machine` peripheral modules are
still in progress.  See the [Roadmap](#-roadmap) for current status.

## 🚀 Getting started

See the [online documentation](https://docs.micropython.org/) for the
MicroPython API reference and general usage information.

### Prerequisites

- A Linux host (native or WSL2).
- `git` and Python 3.8+.
- `source ameba-rtos/env.sh` provisions the cross-toolchain and a Python
  virtualenv on first use — it downloads prebuilts from GitHub (or the Aliyun
  mirror), so the first run needs network; later runs reuse them.

### Get the source

```bash
git clone <repo-url> MicroPython
cd MicroPython
```

### Build (recommended)

A top-level `Makefile` wraps the whole flow — submodule setup, toolchain
environment and the build — into a single command:

```bash
make                            # incremental build
make pristine                   # full (pristine) build
make clean                      # clean
make deploy PORT=/dev/ttyUSB0   # flash (skips build; run make first)
make build deploy PORT=/dev/ttyUSB0  # build, then flash
```

`make` initialises the required submodules on the first run (needs network),
sources the toolchain environment, and builds.  Submodules are re-synced on
every run, so a later `git pull` that bumps a submodule pointer is picked up
automatically.

### Build (manual)

If you prefer to run the steps yourself:

```bash
# One-time: initialise submodules needed for the build
git submodule update --init micropython ameba-rtos
cd micropython
git submodule update --init lib/berkeley-db-1.xx lib/micropython-lib
cd ..

# Each shell session: initialise the toolchain, then build
source ameba-rtos/env.sh
cd ports/ameba-rtos-m
ameba.py build      # or: ameba.py build -p  /  ameba.py clean
```

Either way you should see **`Build done`** when the build succeeds.  The
`mpy-cross` bytecode compiler is built automatically on the first run.

> [!TIP]
> For detailed toolchain setup, build and flashing, see the
> [Ameba-RTOS SDK User Guide](https://aiot.realmcu.com/en/latest/rtos/sdk/index.html).

### Flash and run

The build writes the firmware images to
`ports/ameba-rtos-m/build_RTL8721Dx/` (`boot.bin`, `app.bin`, `ota_all.bin`).
The quickest path is `make deploy`, which flashes without rebuilding — fast
for repeated deploys (the `PORT` is required; it retries automatically on
transient flash errors, and `BAUD=` overrides the rate):

```bash
make deploy PORT=/dev/ttyUSB0            # flash only (skips build)
make build deploy PORT=/dev/ttyUSB0      # build, then flash
make build deploy PORT=/dev/ttyUSB0 BAUD=115200
```

Or flash an existing build manually (run `ameba.py flash -h` for options such
as `-b <baud>` and `--chip-erase`):

```bash
cd ports/ameba-rtos-m
ameba.py flash -p /dev/ttyUSB0
```

Then connect over LOGUART with any serial terminal:

```bash
picocom -b 115200 /dev/ttyACM0
```

> [!IMPORTANT]
> LOGUART runs at **115200 baud** by default — set your terminal to match.
> Change `MICROPY_HW_LOGUART_BAUDRATE` in `src/mpconfigport.h` to use another
> rate (e.g. `1500000`).

Once connected you can use the REPL:

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

## ✨ Features

- Full MicroPython REPL over LOGUART serial, with soft reset and paste mode
- Wi-Fi networking: `network.WLAN` STA and AP modes, with scan, connect,
  ifconfig, and status methods
- BSD sockets over lwIP: TCP/UDP with blocking, non-blocking, timeout, and
  SSL support
- Flash filesystem: `ameba.Flash` block device with a littlefs (LFS2) VFS,
  persistent `boot.py` and `main.py` across power cycles
- Frozen modules: `bundle-networking`, `umqtt`, `dht`, `neopixel` and more
  pre-compiled into the firmware
- Multi-threading: `_thread` module backed by FreeRTOS tasks
- `machine` module: `unique_id()`, `reset_cause()` and expanding peripheral
  APIs

## 🔌 Supported hardware

| SoC                                    | Status        |
|----------------------------------------|---------------|
| AmebaDplus (RTL8721Dx)                 | Active        |
| AmebaLite (RTL8720E / RTL8710E)        | Planned       |
| AmebaGreen2 (RTL8721F)                 | Planned       |
| AmebaSmart (RTL8730E)                  | Planned       |

## 🏗️ Architecture

The port is compiled as an `ameba_add_internal_library(micropython)` component
within the ameba-rtos CMake framework.  The entry point is `app_example()` in
`mp_main.c`, which creates a FreeRTOS task that runs `mp_main()`.

Startup flow:

```text
app_example() --> FreeRTOS task --> mp_main() --> _boot.py --> boot.py --> main.py --> REPL
```

A soft reset re-initialises the GC heap and interpreter via `goto soft_reset`
without restarting the RTOS task.

Repository structure:

```text
MicroPython/
├── ports/
│   └── ameba-rtos-m/          # Port directory (active work area)
│       ├── src/               # Port C sources
│       │   ├── mp_main.c      #   Entry: app_example() -> mp_main() -> REPL
│       │   ├── mphalport.c    #   UART HAL (stdin_ringbuf / LOGUART)
│       │   ├── mpthreadport.c #   _thread -> FreeRTOS task
│       │   ├── network_wlan.c #   network.WLAN (Ameba Wi-Fi API)
│       │   ├── modsocket.c    #   BSD socket (lwIP)
│       │   ├── ameba_flash.c  #   ameba.Flash (VFS block device)
│       │   ├── modmachine.c   #   machine module
│       │   └── mpconfigport.h #   Feature flags / heap size
│       ├── boards/manifest.py #   Frozen module list
│       └── modules/_boot.py   #   Startup script (frozen)
├── micropython/               # MicroPython upstream (submodule, read-only)
└── ameba-rtos/                # Realtek Ameba-RTOS SDK (submodule, read-only)
    ├── ameba.py               #   Unified build CLI entry point
    └── env.sh                 #   Toolchain environment init
```

Key port files:

| File               | Role                                      |
|--------------------|-------------------------------------------|
| `mpconfigport.h`   | All MicroPython feature flags; heap size  |
| `mphalport.c`      | UART I/O via `stdin_ringbuf`, `LOGUART`   |
| `mpthreadport.c`   | Thread support via FreeRTOS tasks         |
| `modameba.c`       | The `ameba` Python module (Flash access)  |
| `modsocket.c`      | BSD socket API                            |
| `network_wlan.c`   | `network.WLAN` STA/AP (Ameba Wi-Fi API)   |

## 🗺️ Roadmap

Phases are listed in implementation order (the `Phase` number is a stable
identifier, not the sequence).

| Phase | Content                                            | Status        |
|:-----:|----------------------------------------------------|:-------------:|
| 0     | Code audit (API residue scan, QSTR completeness)   | Done          |
| 1     | `network` — Wi-Fi STA / AP / scan                  | Done          |
| 1.5   | Flash FS layout fix (`ameba.Flash` + VFS)          | Done          |
| 2     | `machine` — `unique_id()` / `reset_cause()`        | Done          |
| 3     | `machine.Pin` (digital read/write + IRQ)           | Code complete |
| 9     | `machine.Timer`                                    | Code complete |
| 4     | `machine.UART`                                     | Planned       |
| 6     | `machine.I2C`                                      | Planned       |
| 8     | `machine.PWM`                                      | Planned       |
| 7     | `machine.ADC`                                      | Planned       |
| 5     | `machine.SPI`                                      | Planned       |
| 11    | `machine.WDT`                                      | Planned       |
| 10    | `machine.RTC`                                      | Planned       |
| 16    | `ameba.Partition` / OTA                            | In progress   |
| 15    | USB CDC REPL                                       | Planned       |
| 12    | Bluetooth BLE (GAP / GATT)                         | Planned       |
| 13    | `machine.I2S`                                      | Planned       |
| 14    | `machine.SDCard`                                   | Planned       |

*Code complete* means the implementation is merged and builds, with
on-hardware verification still pending.
