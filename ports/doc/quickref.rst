.. _ameba_quickref:

Quick reference for the Ameba port
====================================

Below is a quick reference for Ameba-based boards. If it is your first time
working with one of these boards it may be useful to get an overview of the
chip first:

.. toctree::
   :maxdepth: 1

   general.rst
   tutorial/intro.rst

Installing MicroPython
------------------------

See :ref:`ameba_intro` for the full flashing procedure -- both the
one-step ``make deploy`` build-and-flash and the manual ``AmebaFlash.py``
route -- along with a troubleshooting section.

General board control
-----------------------

The MicroPython REPL is on the board's LOG UART at 115200 baud.
Tab-completion and paste mode (Ctrl-E) work as on every other port.

The :mod:`machine` module::

    import machine

    machine.freq()          # get the KM4 CPU frequency (read-only on this port)
    machine.unique_id()      # 4-byte bytes object, from the chip's eFuse UUID
    machine.reset_cause()    # PWRON_RESET / HARD_RESET / WDT_RESET /
                              # DEEPSLEEP_RESET / SOFT_RESET

Networking
-----------

WLAN
^^^^

The :class:`network.WLAN` class in the :mod:`network` module. The
constructor argument defaults to the station interface if omitted, but
always pass it explicitly to avoid ambiguity between STA and AP::

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
   ``wlan.connect()`` on this port does **not** retry forever. It blocks
   until the connection attempt finishes and raises ``OSError``
   (``ENONET``/``EINVAL``/``EACCES``/``ETIMEDOUT``) on failure instead of
   leaving the interface in a ``STAT_CONNECTING`` state for the caller to
   poll. There is no ``reconnects`` option.

.. note::
   For the STA interface, ``active()`` only tracks a Python-visible flag —
   it does not actually start/stop the WiFi radio (the radio is otherwise
   managed by the SDK). For the AP interface, ``active(True)``/``active(False)``
   really does start/stop the soft-AP.

``wlan.config()`` only supports ``ssid``/``essid``, ``key``/``password``,
``channel``, ``security`` and ``hidden`` as setters (AP only), and
``ssid``/``essid``, ``channel``, ``mac``, ``security`` as getters. There is
no ``max_clients`` option.

``wlan.status()`` with no argument returns one of ``network.WLAN.STAT_IDLE``,
``STAT_CONNECTING``, ``STAT_GOT_IP``, ``STAT_WRONG_PASSWORD``, or
``STAT_HANDSHAKE_TIMEOUT`` for STA, or the number of connected clients for AP.
With a string argument it returns ``"rssi"``/``"data_rssi"``/``"snr"`` (STA)
or ``"stations"`` -- a list of ``{"mac", "ip", "rssi"}`` dicts (AP only).

Once the network is up, the :mod:`socket <socket>` module works as usual,
and :mod:`ssl <ssl>` is available for TLS connections.

Delay and timing
------------------

Use the :mod:`time <time>` module, same as every other port::

    import time

    time.sleep(1)
    time.sleep_ms(500)
    time.sleep_us(10)
    start = time.ticks_ms()
    delta = time.ticks_diff(time.ticks_ms(), start)

Timers
-------

Use the :ref:`machine.Timer <machine.Timer>` class. The ``id`` argument is
cosmetic (only used when printing the object); the port allocates from an
internal pool of hardware timer slots::

    from machine import Timer

    tim = Timer(-1)
    tim.init(period=5000, mode=Timer.ONE_SHOT, callback=lambda t: print('done'))

    tim2 = Timer(-1)
    tim2.init(period=2000, mode=Timer.PERIODIC, callback=lambda t: print('tick'))

``period`` is in milliseconds. Timer callbacks are always scheduled as soft
IRQs; passing ``hard=True`` raises ``ValueError``. Constructing more timers
than the hardware pool provides raises ``OSError``.

Pins and GPIO
--------------

Use the :ref:`machine.Pin <machine.Pin>` class. See
:ref:`Pin naming and numbering <ameba_pin_naming>` for why pin arguments
should be given as board-name strings, not raw integers::

    from machine import Pin

    led = Pin("PA14", Pin.OUT)   # example pin -- use one your board breaks out
    led.on()
    led.off()
    led.toggle()

    btn = Pin("PA13", Pin.IN, Pin.PULL_UP)
    print(btn.value())

    od = Pin("PA15", Pin.OPEN_DRAIN, value=1)  # open-drain output, released (Hi-Z) high

Pin IRQs::

    def handler(pin):
        print('edge on', pin)

    btn.irq(handler, Pin.IRQ_RISING | Pin.IRQ_FALLING)

Constants: ``Pin.IN``, ``Pin.OUT``, ``Pin.OPEN_DRAIN``; ``Pin.PULL_NONE``,
``Pin.PULL_UP``, ``Pin.PULL_DOWN``; ``Pin.IRQ_RISING``, ``Pin.IRQ_FALLING``,
``Pin.IRQ_RISING_FALLING``.

``Pin.board`` only exposes the pins actually broken out on header pins of
the current board; which set that is depends on your board's definition.
Any other valid SoC pin must be addressed by its numeric ``PinName`` value
or is simply not reachable from user code on that board.

UART (serial bus)
-------------------

See :ref:`machine.UART <machine.UART>`. There are two hardware UARTs::

    from machine import UART

    uart1 = UART(1, baudrate=9600)     # defaults to tx=PA_29, rx=PA_28
    uart1.write('hello')
    uart1.read(5)

Both UARTs support fully flexible pinmux, so ``tx=``/``rx=`` can be any
valid SPIC-excluded pin (as int ``PinName`` or a ``"PAx"``/``"PBx"`` string,
not limited to the 19 board-named pins). Defaults: UART(0) tx=``PA_31``
rx=``PA_30`` (this is also the REPL/LOG UART); UART(1) tx=``PA_29``
rx=``PA_28``.

Only ``bits=7`` or ``8``, and ``stop=1`` or ``2`` are supported. ``rxbuf``
is clamped to the range 32-8192 bytes.

RX interrupts and break signalling are supported::

    uart1.irq(lambda u: print('rx'), UART.IRQ_RX)  # only IRQ_RX is available; hard=True not supported
    uart1.sendbreak()

There is no hardware CTS/RTS flow control on this port.

Software SPI bus
------------------

Works on all pins, via the :ref:`machine.SoftSPI <machine.SoftSPI>` class,
same API as every other port::

    from machine import Pin, SoftSPI

    spi = SoftSPI(baudrate=100000, polarity=1, phase=0, sck=Pin("PA13"), mosi=Pin("PA14"), miso=Pin("PA15"))

Hardware SPI bus
------------------

Two hardware SPI buses, accessed via the :ref:`machine.SPI <machine.SPI>`
class. Only ``bits=8`` and ``firstbit=SPI.MSB`` are supported. This port
never drives chip-select for you -- toggle a :class:`machine.Pin` yourself::

    from machine import Pin, SPI

    cs = Pin("PA18", Pin.OUT, value=1)
    spi = SPI(0, 10_000_000)   # SPI(0) defaults: sck=PA_26, mosi=PA_27, miso=PA_28
    cs.value(0)
    spi.write(b'12345')
    cs.value(1)

    spi1 = SPI(1, 1_000_000, sck=Pin("PB17"), mosi=Pin("PB18"), miso=Pin("PB19"))  # SPI(1) has no board default; pins must be given explicitly

.. warning::
   SPI(0)'s default pins (``PA_26``/``PA_27``/``PA_28``) overlap with
   I2C(0)'s default pins (``PA_26``/``PA_27``). If you need both peripherals
   at once, give at least one of them explicit, non-overlapping pins.

Software I2C bus
------------------

Works on all output-capable pins, via :ref:`machine.SoftI2C <machine.SoftI2C>`::

    from machine import Pin, SoftI2C

    i2c = SoftI2C(scl=Pin("PA26"), sda=Pin("PA27"), freq=100000)
    i2c.scan()
    i2c.writeto(0x3a, '12')
    i2c.readfrom(0x3a, 4)

Hardware I2C bus
------------------

Two hardware I2C peripherals, accessed via :ref:`machine.I2C <machine.I2C>`.
Unlike the software version, ``scl=``/``sda=`` **must** be given explicitly
on first construction -- there is no chip-wide default::

    from machine import I2C

    i2c = I2C(0, scl=Pin("PA26"), sda=Pin("PA27"), freq=400000)
    i2c.scan()
    i2c.writeto(0x3a, b'12')

This port does not support a true repeated-START; a combined
``readfrom_mem``/``writeto_mem`` style transaction is issued as two separate
transactions with a STOP in between.

I2C target (slave) mode
^^^^^^^^^^^^^^^^^^^^^^^^^

Either I2C controller can instead run in target/slave mode via
:class:`machine.I2CTarget`, mutually exclusive with using the same
controller as a master::

    from machine import I2CTarget

    target = I2CTarget(0, 0x41)   # defaults to scl=PA_26, sda=PA_27
    target.irq(lambda t, ev: print('event', ev))

Unlike :class:`machine.I2C` (which has no default pins and always requires
``scl=``/``sda=``), :class:`machine.I2CTarget` does have a per-controller
default pin pair; pass ``scl=``/``sda=`` explicitly to override it.

Only 7-bit addressing is supported (``addrsize=10`` raises ``ValueError``).

PWM (pulse width modulation)
------------------------------

8 PWM channels share a single hardware timer, so **all channels run at the
same frequency**; only the duty cycle is independent per channel::

    from machine import Pin, PWM

    pwm0 = PWM(Pin("PA14"), freq=5000, duty_u16=32768)
    pwm0.freq()
    pwm0.freq(1000)          # changes the frequency of every other active PWM channel too
    pwm0.duty_u16(16384)     # 0-65535
    pwm0.duty_ns(250_000)    # pulse width in nanoseconds
    pwm0.deinit()

Only PA_6 and above support PWM. This port does not implement the legacy
0-1023 ``duty()`` method -- use ``duty_u16()`` or ``duty_ns()``.

ADC (analog to digital conversion)
------------------------------------

8 ADC channels, exposed via :ref:`machine.ADC <machine.ADC>`::

    from machine import ADC

    adc = ADC(pin)          # or ADC(channel), channel 0-7
    val = adc.read_u16()    # 12-bit hardware reading, scaled to the 0-65535 range

This port only implements ``read_u16()`` and ``deinit()`` -- there is no
``read()``, ``read_uv()``, ``init()``, ``atten()``, ``width()``, or
``ADCBlock``.

I2S bus
--------

See :ref:`machine.I2S <machine.I2S>`. I2S buses are exposed as SPORT0, and
SPORT1 on chips that provide a second SPORT block; how many are available
depends on the build target::

    from machine import I2S, Pin

    i2s = I2S(0, sck=Pin("PA13"), ws=Pin("PA14"), sd=Pin("PA15"),
              mode=I2S.TX, bits=16, format=I2S.STEREO, rate=44100, ibuf=40000)
    i2s.write(buf)

``bits`` must be 16 or 32; ``format`` must be ``I2S.MONO`` or ``I2S.STEREO``;
``ibuf`` must be at least 256 bytes. Internally RX always uses a 32-bit slot
width regardless of the ``bits`` setting.

Real time clock (RTC)
-----------------------

See :ref:`machine.RTC <machine.RTC>`::

    from machine import RTC

    rtc = RTC()
    rtc.datetime((2026, 1, 1, 0, 12, 0, 0, 0))  # (year, month, day, weekday, hour, minute, second, subsecond)
    rtc.datetime()

The ``weekday`` and ``subsecond`` fields are accepted but ignored -- they
are not written to hardware, and the getter always reports ``subsecond=0``.

RTC alarm and interrupts
-------------------------

``machine.RTC`` supports a hardware alarm and interrupt, in addition to
``datetime()``::

    from machine import RTC

    rtc = RTC()
    rtc.alarm(0, 5000)               # fire once, 5 seconds from now
    rtc.alarm(0, 5000, repeat=True)  # fire every 5 seconds
    rtc.alarm_left(0)                # milliseconds remaining
    rtc.alarm_cancel(0)

    rtc.irq(trigger=rtc.ALARM0, handler=lambda irq: print("alarm!"))

Only alarm id ``0`` (``RTC.ALARM0``) is supported -- the hardware has a
single alarm register. The alarm is a real hardware interrupt, not
software polling; ``alarm()``/``alarm_left()``/``alarm_cancel()``/``irq()``
have all been verified on hardware (both PKE8721DAF and EV8711FLM) to fire
and dispatch correctly while the CPU is running normally (REPL/main loop).
``alarm_left()`` overflows (wraps to a small, wrong value) for a target more
than ~49.7 days away -- avoid arming an alarm that far out.

``irq(wake=...)`` accepts ``machine.IDLE``/``SLEEP``/``DEEPSLEEP`` for API
shape compatibility, but **none of them are currently functional as a wake
source**. Tested on hardware on both boards: an armed RTC alarm does
**not** wake the chip out of a parameterless ``machine.lightsleep()`` --
the call hangs forever, and the board only recovers via a fresh
``make deploy`` (i.e. a re-flash/reset). If you need to sleep for roughly
an alarm interval, pass an explicit duration to ``machine.lightsleep(ms)``
instead of relying on the RTC interrupt to end an indefinite sleep.
``machine.deepsleep()`` cannot be woken by the RTC alarm either (deepsleep
wake sources are limited to the AON timer and AON GPIO pins). Both
limitations are known issues, not yet resolved.

WDT (Watchdog timer)
----------------------

See :ref:`machine.WDT <machine.WDT>`::

    from machine import WDT

    wdt = WDT(timeout=5000)   # 1-65535 ms
    wdt.feed()

Once started, the WDT **cannot be disabled by software** -- there is no
``deinit()``. Re-constructing ``WDT()`` reconfigures and re-arms the same
underlying instance.

machine.bitstream / NeoPixel driver
--------------------------------------

Use the ``neopixel`` module, same API as other ports::

    from machine import Pin
    from neopixel import NeoPixel

    pin = Pin("PA14", Pin.OUT)
    np = NeoPixel(pin, 8)
    np[0] = (255, 255, 255)
    np.write()

On chips that have a compatible LEDC peripheral, ``machine.bitstream``
transparently uses a hardware-DMA backend (LEDC driving GDMA) whenever the
buffer is a multiple of 3 bytes (RGB, not RGBW), there are at most 1024
LEDs, and a GDMA channel is available; it otherwise falls back to a
software bit-banged implementation with interrupts disabled. GDMA channels
are a shared pool, so heavy concurrent use by another peripheral (e.g.
:class:`machine.I2S`) can occasionally force the software fallback even
when the buffer/LED-count conditions are met. Both paths are transparent to
``NeoPixel``/``APA106`` callers. Chips without a compatible LEDC driver
always use the software fallback.

OneWire and DHT driver
------------------------

Implemented in software, works on any pin, frozen into this port's firmware
(see ``boards/manifest.py``)::

    from machine import Pin
    import onewire, ds18x20, dht

    ow = onewire.OneWire(Pin("PA14"))
    ow.scan()

    d = dht.DHT22(Pin("PA14"))
    d.measure()
    d.temperature()
    d.humidity()

Deep-sleep and light-sleep
-----------------------------

::

    import machine

    if machine.reset_cause() == machine.DEEPSLEEP_RESET:
        print('woke from deep sleep')

    machine.lightsleep(1000)   # returns after ~1s (or wake event); no argument sleeps indefinitely
    machine.deepsleep(10000)   # ms, clamped to a 30s hardware maximum; waking resets the chip

``machine.wake_reason()`` reports the wake source. After ``deepsleep()`` it
returns ``machine.PIN_WAKE`` or ``machine.TIMER_WAKE``. After
``lightsleep()`` it returns ``machine.TIMER_WAKE`` if a duration was given,
or ``0`` if not -- lightsleep does not distinguish a pin event from a plain
timeout.

Entering the bootloader
--------------------------

::

    import machine
    machine.bootloader()   # resets into UART download mode (for reflashing)

Multiplexed REPL (os.dupterm)
--------------------------------

``os.dupterm()``/``os.dupterm_notify()`` are available (standard
:mod:`os` API) to duplicate the REPL onto another stream, e.g. WebREPL or a
second UART.

ameba module: Flash and OTA
------------------------------

The port-specific ``ameba`` module exposes raw flash access and an OTA
updater::

    import ameba

    f = ameba.Flash()        # singleton; covers the VFS1 flash partition
    f.start, f.len, f.block_size

    ota = ameba.OTA()
    ota.write(chunk)          # feed firmware image data, may be called repeatedly
    ota.finish()              # validates and activates the new image
    ameba.OTA.cur_slot()      # 1 or 2 -- classmethod, which OTA slot is currently running
    machine.reset()           # apply the new firmware

Only one ``ameba.OTA()`` can be in progress at a time; constructing a second
one while the first is unfinished raises ``OSError(EBUSY)``.
