.. _ameba_quickref_zh:

Ameba port 快速参考
======================

下面是 Ameba 系列板卡的快速参考。如果这是你第一次接触这类板卡，不妨先看看芯片
概览：

.. toctree::
   :maxdepth: 1

   general.rst
   tutorial/intro.rst

安装 MicroPython
------------------

完整的刷机流程见 :ref:`ameba_intro_zh`——既有一步搞定"编译 + 刷写"的
``make deploy``，也有手动用 ``AmebaFlash.py`` 的路子，还附了一节排障。

板卡基本控制
-------------

MicroPython 的 REPL 在板卡的 LOG UART 上，波特率 115200。Tab 补全和粘贴模式
（Ctrl-E）跟其他 port 一样可用。

:mod:`machine` 模块::

    import machine

    machine.freq()          # get the KM4 CPU frequency (read-only on this port)
    machine.unique_id()      # 4-byte bytes object, from the chip's eFuse UUID
    machine.reset_cause()    # PWRON_RESET / HARD_RESET / WDT_RESET /
                              # DEEPSLEEP_RESET / SOFT_RESET

网络
-----

WLAN
^^^^

:mod:`network` 模块里的 :class:`network.WLAN` 类。构造函数不传接口参数时会
默认成 station 接口，但为了避免 STA/AP 混淆，建议始终显式传入::

    import network

    wlan = network.WLAN(network.WLAN.IF_STA)   # station interface
    wlan.active(True)
    wlan.scan()                  # [(ssid, bssid, channel, rssi, security, hidden, wireless_mode), ...]
    wlan.isconnected()
    wlan.connect('ssid', 'key')  # blocks; raises OSError on failure (see below)
    wlan.config('mac')
    wlan.ifconfig()               # (ip, netmask, gw, dns) -- this port implements the
                                   # classic ifconfig() API, not network.WLAN.ipconfig()

    ap = network.WLAN(network.WLAN.IF_AP)
    ap.config(ssid='Ameba-AP')             # AP config setters only; STA raises TypeError
    ap.config(key='password', security=network.WLAN.SEC_WPA2)
    ap.active(True)                        # actually starts/stops the softAP

.. note::
   本 port 上的 ``wlan.connect()`` **不会** 无限重试。它会阻塞直到这次连接尝试
   有结果，失败时抛出 ``OSError``
   （``ENONET``/``EINVAL``/``EACCES``/``ETIMEDOUT``），不会把接口留在
   ``STAT_CONNECTING`` 状态让调用者去轮询。也没有 ``reconnects`` 选项。

.. note::
   对 STA 接口来说，``active()`` 只是维护一个 Python 侧可见的标志位——并不会真的
   启停 WiFi 射频（射频本身由 SDK 另行管理）。对 AP 接口来说，
   ``active(True)``/``active(False)`` 才是真正启停 soft-AP。

``wlan.config()`` 作为 setter 只支持 ``ssid``/``essid``、``key``/``password``、
``channel``、``security`` 和 ``hidden`` （仅 AP），作为 getter 只支持
``ssid``/``essid``、``channel``、``mac``、``security``。没有 ``max_clients``
选项。

``wlan.status()`` 不带参数时，STA 返回 ``network.WLAN.STAT_IDLE``、
``STAT_CONNECTING``、``STAT_GOT_IP``、``STAT_WRONG_PASSWORD``、
``STAT_HANDSHAKE_TIMEOUT`` 之一，AP 返回已连接客户端数量。带字符串参数时，STA
支持 ``"rssi"``/``"data_rssi"``/``"snr"``，AP 支持 ``"stations"`` —— 返回一个
``{"mac", "ip", "rssi"}`` 字典组成的列表。

网络起来之后，:mod:`socket <socket>` 模块可以照常使用，:mod:`ssl <ssl>` 也可用
于 TLS 连接。

延时与计时
-----------

用 :mod:`time <time>` 模块，跟其他 port 一样::

    import time

    time.sleep(1)
    time.sleep_ms(500)
    time.sleep_us(10)
    start = time.ticks_ms()
    delta = time.ticks_diff(time.ticks_ms(), start)

定时器
-------

用 :ref:`machine.Timer <machine.Timer>` 类。``id`` 参数只是装饰性的（只在打印对象
时用得上），实际分配来自内部的硬件定时器槽位池::

    from machine import Timer

    tim = Timer(-1)
    tim.init(period=5000, mode=Timer.ONE_SHOT, callback=lambda t: print('done'))

    tim2 = Timer(-1)
    tim2.init(period=2000, mode=Timer.PERIODIC, callback=lambda t: print('tick'))

``period`` 单位是毫秒。定时器回调始终以 soft IRQ 方式调度；传 ``hard=True`` 会
抛出 ``ValueError``。构造超过硬件池容量的定时器会抛出 ``OSError``。

引脚与 GPIO
------------

用 :ref:`machine.Pin <machine.Pin>` 类。关于为什么引脚参数应该用板卡名字符串而不是
裸整数，见 :ref:`引脚命名与编号 <ameba_pin_naming_zh>`::

    from machine import Pin

    led = Pin("PA14", Pin.OUT)   # example pin -- use one your board breaks out
    led.on()
    led.off()
    led.toggle()

    btn = Pin("PA13", Pin.IN, Pin.PULL_UP)
    print(btn.value())

    od = Pin("PA15", Pin.OPEN_DRAIN, value=1)  # open-drain output, released (Hi-Z) high

引脚中断::

    def handler(pin):
        print('edge on', pin)

    btn.irq(handler, Pin.IRQ_RISING | Pin.IRQ_FALLING)

常量：``Pin.IN``、``Pin.OUT``、``Pin.OPEN_DRAIN``；``Pin.PULL_NONE``、
``Pin.PULL_UP``、``Pin.PULL_DOWN``；``Pin.IRQ_RISING``、``Pin.IRQ_FALLING``、
``Pin.IRQ_RISING_FALLING``。

``Pin.board`` 只暴露当前板卡实际引到排针上的引脚，具体是哪一套取决于你那块板卡
的定义。其他合法的 SoC 引脚必须用数字形式的 ``PinName`` 值来访问，或者在那块
板卡上根本没有暴露给用户代码。

UART（串口）
-------------

见 :ref:`machine.UART <machine.UART>`。有两路硬件 UART::

    from machine import UART

    uart1 = UART(1, baudrate=9600)     # defaults to tx=PA_29, rx=PA_28
    uart1.write('hello')
    uart1.read(5)

两路 UART 都支持完全灵活的 pinmux，所以 ``tx=``/``rx=`` 可以是任意合法的（排除
SPIC 相关引脚）引脚（用 int ``PinName`` 或者 ``"PAx"``/``"PBx"`` 字符串，不限于那
19 个板卡具名引脚）。默认值：UART(0) tx=``PA_31`` rx=``PA_30`` （这也是
REPL/LOG UART）；UART(1) tx=``PA_29`` rx=``PA_28``。

只支持 ``bits=7`` 或 ``8``，以及 ``stop=1`` 或 ``2``。``rxbuf`` 会被钳制在
32-8192 字节的范围内。

支持 RX 中断和 break 信号::

    uart1.irq(lambda u: print('rx'), UART.IRQ_RX)  # only IRQ_RX is available; hard=True not supported
    uart1.sendbreak()

本 port 没有硬件 CTS/RTS 流控。

软件 SPI 总线
--------------

在任意引脚上都能用，通过 :ref:`machine.SoftSPI <machine.SoftSPI>` 类，API 和其他
port 一样::

    from machine import Pin, SoftSPI

    spi = SoftSPI(baudrate=100000, polarity=1, phase=0, sck=Pin("PA13"), mosi=Pin("PA14"), miso=Pin("PA15"))

硬件 SPI 总线
--------------

两路硬件 SPI 总线，通过 :ref:`machine.SPI <machine.SPI>` 类访问。只支持
``bits=8`` 和 ``firstbit=SPI.MSB``。本 port 不会替你驱动片选——自己用一个
:class:`machine.Pin` 来控制::

    from machine import Pin, SPI

    cs = Pin("PA18", Pin.OUT, value=1)
    spi = SPI(0, 10_000_000)   # SPI(0) defaults: sck=PA_26, mosi=PA_27, miso=PA_28
    cs.value(0)
    spi.write(b'12345')
    cs.value(1)

    spi1 = SPI(1, 1_000_000, sck=Pin("PB17"), mosi=Pin("PB18"), miso=Pin("PB19"))  # SPI(1) has no board default; pins must be given explicitly

.. warning::
   SPI(0) 的默认引脚（``PA_26``/``PA_27``/``PA_28``）和 I2C(0) 的默认引脚
   （``PA_26``/``PA_27``）是重叠的。如果两个外设要同时用，至少给其中一个显式指定
   不冲突的引脚。

软件 I2C 总线
--------------

在任意输出能力的引脚上都能用，通过 :ref:`machine.SoftI2C <machine.SoftI2C>`::

    from machine import Pin, SoftI2C

    i2c = SoftI2C(scl=Pin("PA26"), sda=Pin("PA27"), freq=100000)
    i2c.scan()
    i2c.writeto(0x3a, '12')
    i2c.readfrom(0x3a, 4)

硬件 I2C 总线
--------------

两路硬件 I2C 外设，通过 :ref:`machine.I2C <machine.I2C>` 访问。和软件版本不同的
是，首次构造时 **必须** 显式传入 ``scl=``/``sda=``——没有芯片级默认值::

    from machine import I2C

    i2c = I2C(0, scl=Pin("PA26"), sda=Pin("PA27"), freq=400000)
    i2c.scan()
    i2c.writeto(0x3a, b'12')

本 port 不支持真正的 repeated-START；``readfrom_mem``/``writeto_mem`` 这类合并
事务，会被拆成两次独立事务，中间插一个 STOP。

I2C target（从机）模式
^^^^^^^^^^^^^^^^^^^^^^^^

两路 I2C 控制器都可以改跑 target/从机模式，用 :class:`machine.I2CTarget`，和把
同一个控制器当主机用是互斥的::

    from machine import I2CTarget

    target = I2CTarget(0, 0x41)   # defaults to scl=PA_26, sda=PA_27
    target.irq(lambda t, ev: print('event', ev))

跟 :class:`machine.I2C`（没有默认引脚，永远要求显式传 ``scl=``/``sda=``）不同，
:class:`machine.I2CTarget` 每个控制器都有一套默认引脚；要覆盖的话显式传
``scl=``/``sda=`` 即可。

只支持 7 位地址（``addrsize=10`` 会抛出 ``ValueError``）。

PWM（脉宽调制）
----------------

8 个 PWM 通道共用一个硬件定时器，所以**所有通道频率都一样**；只有占空比是每个
通道独立的::

    from machine import Pin, PWM

    pwm0 = PWM(Pin("PA14"), freq=5000, duty_u16=32768)
    pwm0.freq()
    pwm0.freq(1000)          # changes the frequency of every other active PWM channel too
    pwm0.duty_u16(16384)     # 0-65535
    pwm0.duty_ns(250_000)    # pulse width in nanoseconds
    pwm0.deinit()

只有 PA_6 及以上的引脚支持 PWM。本 port 没有实现传统的 0-1023 ``duty()`` 方法——
用 ``duty_u16()`` 或 ``duty_ns()``。

ADC（模数转换）
----------------

8 个 ADC 通道，通过 :ref:`machine.ADC <machine.ADC>` 暴露::

    from machine import ADC

    adc = ADC(pin)          # or ADC(channel), channel 0-7
    val = adc.read_u16()    # 12-bit hardware reading, scaled to the 0-65535 range

本 port 只实现了 ``read_u16()`` 和 ``deinit()`` —— 没有 ``read()``、
``read_uv()``、``init()``、``atten()``、``width()``，也没有 ``ADCBlock``。

I2S 总线
---------

见 :ref:`machine.I2S <machine.I2S>`。I2S 总线以 SPORT0 的形式暴露，部分芯片
还提供第二路 SPORT1；具体有几路取决于编译目标::

    from machine import I2S, Pin

    i2s = I2S(0, sck=Pin("PA13"), ws=Pin("PA14"), sd=Pin("PA15"),
              mode=I2S.TX, bits=16, format=I2S.STEREO, rate=44100, ibuf=40000)
    i2s.write(buf)

``bits`` 必须是 16 或 32；``format`` 必须是 ``I2S.MONO`` 或 ``I2S.STEREO``；
``ibuf`` 至少要 256 字节。内部实现上，RX 方向不管 ``bits`` 设成多少，都固定用
32 位的 slot 宽度。

实时时钟（RTC）
----------------

见 :ref:`machine.RTC <machine.RTC>`::

    from machine import RTC

    rtc = RTC()
    rtc.datetime((2026, 1, 1, 0, 12, 0, 0, 0))  # (year, month, day, weekday, hour, minute, second, subsecond)
    rtc.datetime()

``weekday`` 和 ``subsecond`` 这两个字段会被接受但直接忽略——不会写入硬件，
getter 读回来的 ``subsecond`` 永远是 0。

WDT（看门狗定时器）
---------------------

见 :ref:`machine.WDT <machine.WDT>`::

    from machine import WDT

    wdt = WDT(timeout=5000)   # 1-65535 ms
    wdt.feed()

一旦启动，WDT **不能用软件关闭**——没有 ``deinit()``。重新构造 ``WDT()`` 会
重新配置并重新武装同一个底层实例。

machine.bitstream / NeoPixel 驱动
------------------------------------

用 ``neopixel`` 模块，API 和其他 port 一样::

    from machine import Pin
    from neopixel import NeoPixel

    pin = Pin("PA14", Pin.OUT)
    np = NeoPixel(pin, 8)
    np[0] = (255, 255, 255)
    np.write()

在带有兼容 LEDC 外设的芯片上，只要缓冲区长度是 3 的倍数（RGB，不是 RGBW）、LED
数量不超过 1024、并且还有空闲的 GDMA 通道，``machine.bitstream`` 就会透明地走
硬件 DMA 后端（LEDC 驱动 GDMA）；否则会退回到关中断的软件 bit-bang 实现。GDMA
通道是共享池，如果有其他外设（比如 :class:`machine.I2S`）同时大量占用，即使
缓冲区/LED 数量条件都满足，也可能偶尔被逼退回软件路径。这两条路径对
``NeoPixel``/``APA106`` 的调用方来说完全透明。没有兼容 LEDC 驱动的芯片始终走
软件回退路径。

OneWire 和 DHT 驱动
---------------------

用软件实现，能在任意引脚上跑，已经冻结进本 port 的固件里（见
``boards/manifest.py``）::

    from machine import Pin
    import onewire, ds18x20, dht

    ow = onewire.OneWire(Pin("PA14"))
    ow.scan()

    d = dht.DHT22(Pin("PA14"))
    d.measure()
    d.temperature()
    d.humidity()

深睡与轻睡
-----------

::

    import machine

    if machine.reset_cause() == machine.DEEPSLEEP_RESET:
        print('woke from deep sleep')

    machine.lightsleep(1000)   # returns after ~1s (or wake event); no argument sleeps indefinitely
    machine.deepsleep(10000)   # ms, clamped to a 30s hardware maximum; waking resets the chip

``machine.wake_reason()`` 会报告唤醒来源。``deepsleep()`` 之后返回
``machine.PIN_WAKE`` 或 ``machine.TIMER_WAKE``；``lightsleep()`` 之后，给了
时长参数就返回 ``machine.TIMER_WAKE``，没给就返回 ``0``——lightsleep 不区分
"引脚事件唤醒"和"单纯超时"。

进入 bootloader
-----------------

::

    import machine
    machine.bootloader()   # resets into UART download mode (for reflashing)

多路复用 REPL（os.dupterm）
------------------------------

``os.dupterm()``/``os.dupterm_notify()`` 都可以用（标准 :mod:`os` API），用来把
REPL 复制到另一个流上，比如 WebREPL 或者第二路 UART。

ameba 模块：Flash 与 OTA
--------------------------

port 专属的 ``ameba`` 模块提供了原始 flash 访问和一个 OTA 升级器::

    import ameba

    f = ameba.Flash()        # singleton; covers the VFS1 flash partition
    f.start, f.len, f.block_size

    ota = ameba.OTA()
    ota.write(chunk)          # feed firmware image data, may be called repeatedly
    ota.finish()              # validates and activates the new image
    ameba.OTA.cur_slot()      # 1 or 2 -- classmethod, which OTA slot is currently running
    machine.reset()           # apply the new firmware

同一时间只能有一个 ``ameba.OTA()`` 在进行中；上一个还没结束就构造第二个会抛出
``OSError(EBUSY)``。
