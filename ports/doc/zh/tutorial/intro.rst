.. _ameba_intro_zh:

Ameba 上手 MicroPython
========================

开始吧！

.. toctree::
    :maxdepth: 1

    reset.rst

前置要求
---------

你需要一块搭载 Ameba RTOS 芯片的板卡。某一份固件面向哪颗芯片是编译期的选择；
本 port 支持的 Ameba 家族成员见 :ref:`ameba_general_zh`。

刷写固件
---------

本 port 用 Realtek 的 **AmebaFlash** 工具刷写，该工具随
`ameba-rtos SDK <https://github.com/Ameba-AIoT/ameba-rtos>`_ 一起发布。

开始之前
^^^^^^^^^

先关掉所有连着这块板卡的串口终端（``mpremote``、``screen``、PuTTY、miniterm 等），
否则串口被占用，刷写会失败。

进入下载模式
^^^^^^^^^^^^^

板卡通常会由 USB 转 UART 芯片的 DTR/RTS 信号自动进入下载模式。如果自动进入失败，
就手动进：**按住** ``DOWNLOAD`` 键，**按一下再松开** ``RESET`` 键，然后**松开**
``DOWNLOAD`` 键。此时板卡就在等待固件镜像了。

方式一 —— ``make deploy``\ （已 checkout 本 port 时）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

如果你已经 checkout 了本 port，仓库根目录的 ``Makefile`` 能一步完成"编译 + 刷写"。
串口必须显式传入::

    make deploy PORT=/dev/ttyUSB0                # 刷写当前构建
    make build deploy PORT=/dev/ttyUSB0          # 先编译再刷写
    make deploy PORT=/dev/ttyUSB0 BAUD=115200    # 覆盖刷写波特率

目标芯片会从构建配置里自动读出，你不用手动去选芯片 profile。

方式二 —— ``AmebaFlash.py``\ （只有固件镜像时）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

如果你手上只有一个 ``firmware.bin``，就用工具直接刷。先装依赖——
``pip install pyserial``（Windows 上还要 ``pyDes``）——然后在
``ameba-rtos/tools/ameba/Flash`` 目录下运行::

    python3 AmebaFlash.py \
        --download \
        --port /dev/ttyUSB0 \
        --baudrate 1500000 \
        --profile Devices/Profiles/<your-SoC>.rdev \
        --image /path/to/firmware.bin \
        --start-address 0x8000000 \
        --memory-type nor

把 ``/dev/ttyUSB0`` 换成你的串口（Windows 上是设备管理器里的 ``COMx``），把
``<your-SoC>`` 换成对应你芯片的 profile。在 WSL 上先用 ``usbipd`` 把板卡挂进来；
刷写过程中连接是稳定的，因为 USB 转 UART 芯片和目标 MCU 是独立供电的。

排障
^^^^

* **端口被占用** —— 还有串口终端连着板卡，关掉再试。
* **下载模式超时** —— DTR/RTS 自动进入失败了，按上面的办法手动进下载模式，然后
  立刻重跑命令。
* **刷写中途出错** —— 一次瞬时通信错误。板卡会自己重启，等几秒让 USB 设备重新
  出现，再重试。
* **慢或不稳定** —— 换个更低的 ``--baudrate``（比如 ``115200``）。

串口终端
---------

刷好之后，REPL 在板卡的 LOG UART 上可用，波特率 115200。用任意串口终端软件连接
即可（``mpremote``、``screen``、PuTTY 等）。
