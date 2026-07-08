恢复出厂设置
=============

如果出了意外，你的 Ameba 板卡不能正常启动 MicroPython 了，可能需要恢复出厂设置。
详见 :ref:`soft_bricking`。

对本 port 做恢复出厂设置，意味着完全擦除 flash（固件和文件系统都擦掉）再重新刷写，
所以之后需要重新刷入 MicroPython，并把 Python 文件重新拷回文件系统。

1. 按照 :ref:`ameba_intro_zh` 里的说明，装好 **AmebaFlash** 并让板卡进入下载
   模式。
2. 擦除整颗 flash 芯片::

       cd ameba-rtos/tools/ameba/Flash
       python3 AmebaFlash.py --chip-erase --port /dev/ttyUSB0 --baudrate 1500000 \
           --profile Devices/Profiles/<your-SoC>.rdev   # 和刷写时用的芯片 profile 相同

3. 按照 :ref:`ameba_intro_zh` 里正常的刷机流程，重新刷入 MicroPython 固件。
