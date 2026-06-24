# cmake configuration for PKE8721DAF (RTL8721DAF / AmebaDplus)
# This file is included by src/CMakeLists.txt when BOARD=PKE8721DAF.

# SoC name passed to ameba.py build <SOC> — read by the port Makefile.
set(AMEBA_BOARD_SOC "RTL8721Dx")

# Inject MicroPython-side board/MCU name macros.
# mpconfigboard.h uses #ifndef guards so these are the authoritative values.
list(APPEND MICROPY_DEF_BOARD
    MICROPY_HW_BOARD_NAME="PKE8721DAF"
    MICROPY_HW_MCU_NAME="RTL8721DAF"
)
