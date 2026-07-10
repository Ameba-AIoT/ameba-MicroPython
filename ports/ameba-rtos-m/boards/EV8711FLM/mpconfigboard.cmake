# cmake configuration for EV8711FLM (RTL8711FLM / AmebaGreen2)
# This file is included by src/CMakeLists.txt when BOARD=EV8711FLM.
#
# Board is Realtek's EV721FL0 evaluation board (UG1002), populated with an
# RTL8711FLM chip. SoC name is confirmed against `ameba.py soc`: RTL8721F and
# RTL8711F both map to the amebagreen2 SoC tree, so the proven RTL8721F build
# target is kept here rather than switching to the also-valid RTL8711F entry.

# SoC name passed to ameba.py build <SOC> — read by the port Makefile.
set(AMEBA_BOARD_SOC "RTL8721F")

# Inject MicroPython-side board/MCU name macros.
# mpconfigboard.h uses #ifndef guards so these are the authoritative values.
list(APPEND MICROPY_DEF_BOARD
    MICROPY_HW_BOARD_NAME="EV8711FLM"
    MICROPY_HW_MCU_NAME="RTL8711FLM"
)
