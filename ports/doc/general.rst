.. _ameba_general:

General information about the Ameba port
=========================================

Ameba RTOS is Realtek's family of WiFi (and, on most members, Bluetooth LE)
enabled System-on-Chip (SoC) devices. This MicroPython port targets the
Ameba RTOS SDK's unified build system (``ameba.py``); which chip in the
family a given firmware runs on is a build-time choice (see below).

Multiple chips in the Ameba family
-----------------------------------

This port runs across several chips in the Ameba family. Capabilities and
pin availability differ from chip to chip, so a given firmware image is
built for one specific chip and board.

The build target is selected via ``soc.name`` in
``ports/ameba-rtos-m/soc_info.json``; this is a build-time choice, not
something switched at runtime.

Because of this, the concrete numbers below (pin counts, peripheral counts,
default pin assignments) describe whichever chip and board your firmware was
built for; on a different chip they may differ. The names printed on your
board are always the authoritative reference.

Technical specifications and SoC datasheets
---------------------------------------------

For chip and module datasheets across the Ameba family — architecture,
clock speeds, memory, peripheral counts, package and pinout diagrams —
see the vendor's datasheet index: https://aiot.realmcu.com/zh/datasheet/index.html
They are the primary reference for chip capabilities and pinout; for
board-specific details (which pins are actually broken out, module
placement, etc.) also keep your board's own datasheet and schematic at
hand.

The following is what *this* MicroPython port actually exposes to Python —
not the chip's full set of capabilities:

* GPIO numbering follows the SDK's ``PinName`` scheme rather than a
  zero-based GPIO index — see `Pin naming and numbering`_ below.
* Peripherals exposed by this port (counts and defaults reflect the build
  target — see `Multiple chips in the Ameba family`_): 2x UART, 2x hardware
  I2C (each of which can also run in target/slave mode), 2x hardware SPI,
  one 8-channel PWM block (all channels share a single frequency), one
  8-channel ADC, 2x I2S (SPORT0/SPORT1), a pool of hardware timers, and a
  hardware watchdog.

.. _ameba_pin_naming:

Pin naming and numbering
-------------------------

``machine.Pin`` numbers on this port are **not** a simple zero-based GPIO
index. Internally every pin is identified by the SDK's ``PinName`` enum,
where port A pins occupy indices 0-31 (``PA_0`` = 0, ``PA_1`` = 1, ...
``PA_31`` = 31) and port B pins occupy indices 32-63 (``PB_0`` = 32, ...
``PB_31`` = 63). Passing that raw integer to ``Pin()`` works, but it means
e.g. ``Pin(6)`` refers to ``PA_6``, not "GPIO6", and most of ``PA_0``-
``PA_5`` (indices 0-5) are rejected outright: these are the pins the SoC's
SPI flash controller (SPIC) uses for the SPI NOR flash built into the
module, so they're never available as general-purpose GPIO.

For this reason, always prefer one of the two named forms instead of a raw
integer:

* ``Pin("PA14")`` — pass the pin name as a string. This matches the label
  printed right on the board header — the pin marked ``PA16`` on the
  silkscreen is ``Pin("PA16")`` in code, no lookup table needed.
* ``Pin(Pin.board.PA14)`` — the same names, as attributes on ``Pin.board``
  instead of strings.

Both forms are board-specific, not just chip-specific: the set of names
they accept comes from whichever board this firmware was built for, and a
different board can expose a different set. Use the names printed on the
board you're actually running.

An out-of-range or otherwise invalid pin (including any of ``PA_0``-``PA_5``)
raises ``ValueError``, it does not silently do nothing.

Software vs. hardware peripherals
----------------------------------

Several buses are available in both a hardware-accelerated form and a
bit-banged software form that works on (almost) any GPIO pin:
:class:`machine.SPI` / :class:`machine.SoftSPI`, and :class:`machine.I2C` /
:class:`machine.SoftI2C`. Use the hardware class when your pins line up
with the peripheral's fixed or default routing, and the ``Soft*`` class
when you need arbitrary pins or more than the two available hardware bus
instances.

Underlying framework
----------------------

MicroPython on this port runs as a task on top of the
`ameba-rtos SDK <https://github.com/Ameba-AIoT/ameba-rtos>`_, Realtek's
FreeRTOS-based development framework for the Ameba family. The SDK
provides the peripheral drivers, networking stack, and build system
(``ameba.py``) that this port is built against.
