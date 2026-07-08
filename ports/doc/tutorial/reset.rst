Factory reset
=============

If something unexpected happens and your Ameba-based board no longer boots
MicroPython, you may have to factory reset it. For more details, see
:ref:`soft_bricking`.

Factory resetting this port involves fully erasing the flash (firmware and
filesystem) and reflashing, so you will need to reflash MicroPython and copy
any Python files to the filesystem again afterwards.

1. Get **AmebaFlash** installed and your board into download mode, as
   described under :ref:`ameba_intro`.
2. Erase the entire flash chip::

       cd ameba-rtos/tools/ameba/Flash
       python3 AmebaFlash.py --chip-erase --port /dev/ttyUSB0 --baudrate 1500000 \
           --profile Devices/Profiles/<your-SoC>.rdev   # same chip profile you flash with

3. Reflash the MicroPython firmware following the normal flashing procedure
   under :ref:`ameba_intro`.
