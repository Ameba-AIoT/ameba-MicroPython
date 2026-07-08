.. _ameba_intro:

Getting started with MicroPython on Ameba
============================================

Let's get started!

.. toctree::
    :maxdepth: 1

    reset.rst

Requirements
-------------

You need a board carrying an Ameba RTOS chip. Which chip a given firmware
targets is a build-time choice; see :ref:`ameba_general` for the Ameba
family members this port supports.

Flashing the firmware
-----------------------

This port is flashed with Realtek's **AmebaFlash** tool, which ships with
the `ameba-rtos SDK <https://github.com/Ameba-AIoT/ameba-rtos>`_.

Before you start
^^^^^^^^^^^^^^^^^

Close any serial terminal (``mpremote``, ``screen``, PuTTY, miniterm, ...)
that is connected to the board. Otherwise the serial port is busy and
flashing fails.

Enter download mode
^^^^^^^^^^^^^^^^^^^^

The board normally enters download mode automatically, driven by the
DTR/RTS lines of its USB-to-UART chip. If that fails, enter it by hand:
**hold** the ``DOWNLOAD`` key, **press and release** the ``RESET`` key,
then **release** the ``DOWNLOAD`` key. The board is now waiting for a
firmware image.

Option 1 -- ``make deploy`` (from a port checkout)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If you have this port checked out, the top-level ``Makefile`` builds the
image and flashes it in one step. Pass the serial port explicitly::

    make deploy PORT=/dev/ttyUSB0                # flash the current build
    make build deploy PORT=/dev/ttyUSB0          # build first, then flash
    make deploy PORT=/dev/ttyUSB0 BAUD=115200    # override the flash baud rate

The target chip is read automatically from the build configuration, so you
never pick a chip profile by hand.

Option 2 -- ``AmebaFlash.py`` (from a firmware image)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If you only have a ``firmware.bin``, flash it with the tool directly.
Install its dependency first -- ``pip install pyserial`` (on Windows also
``pyDes``) -- then run, from ``ameba-rtos/tools/ameba/Flash``::

    python3 AmebaFlash.py \
        --download \
        --port /dev/ttyUSB0 \
        --baudrate 1500000 \
        --profile Devices/Profiles/<your-SoC>.rdev \
        --image /path/to/firmware.bin \
        --start-address 0x8000000 \
        --memory-type nor

Replace ``/dev/ttyUSB0`` with your serial port (a ``COMx`` name on Windows,
from Device Manager) and ``<your-SoC>`` with the profile that matches your
chip. On WSL, attach the board with ``usbipd`` first; the connection stays
stable during flashing because the USB-to-UART chip is powered separately
from the target MCU.

Troubleshooting
^^^^^^^^^^^^^^^

* **Port busy** -- a serial terminal is still connected to the board; close
  it and retry.
* **Download-mode timeout** -- automatic DTR/RTS entry failed; enter
  download mode by hand (see above) and re-run the command immediately.
* **Flash error mid-way** -- a transient communication error. The board
  reboots on its own, so wait a few seconds for the USB device to reappear,
  then retry.
* **Slow or unreliable** -- try a lower ``--baudrate`` (e.g. ``115200``).

Serial prompt
--------------

Once flashed, the REPL is available on the board's LOG UART at 115200 baud.
Connect with any serial terminal (``mpremote``, ``screen``, PuTTY, etc.).
