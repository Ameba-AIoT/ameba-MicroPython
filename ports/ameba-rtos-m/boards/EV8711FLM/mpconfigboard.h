#ifndef MICROPY_INCLUDED_AMEBA_BOARDS_EV8711FLM_MPCONFIGBOARD_H
#define MICROPY_INCLUDED_AMEBA_BOARDS_EV8711FLM_MPCONFIGBOARD_H

#define MICROPY_HW_BOARD_NAME   "EV8711FLM"
#define MICROPY_HW_MCU_NAME     "RTL8711FLM"

// LOGUART (REPL/debug UART) is a fixed, ROM-configured peripheral, not a
// board-configurable machine.UART(): PA2 = UART_LOG_RXD, PB20 = UART_LOG_TXD
// (Realtek UG1002 EV721FL0 EVB User Guide v1.2, section 4.4). No macro to
// set here -- listed for reference only.
//
// No schematic is available for this EVB's User LED (LEDR/LEDG/LEDB), so those
// macros are intentionally left unset rather than guessed.

// Default peripheral pins for machine.UART / machine.SPI / machine.I2CTarget
// when the user gives no pin arguments.  AmebaGreen2 has a full soft pin-mux
// (any GPIO can carry any of these functions -- confirmed against the RTL8711F
// pinmux table), so these are chosen for convenience: all are directly-wired
// (not "default NC") header pins on this EVB and avoid the flash (PC_2..PC_7),
// power (PA_0/PA_1) and LOGUART (PA_2/PB_20) pins.  Users typically override
// them per call; tune here if a different default wiring is preferred.
// machine.UART(0/1) default TX/RX:
#define MICROPY_HW_UART0_TX  (PA_31)   // J22 pin 2 (direct)
#define MICROPY_HW_UART0_RX  (PA_30)   // J22 pin 3 (direct)
#define MICROPY_HW_UART1_TX  (PA_6)    // J20 pin 26 (direct)
#define MICROPY_HW_UART1_RX  (PA_7)    // J20 pin 27 (direct)
// machine.SPI(0) default pins (J20, adjacent, directly wired):
#define MICROPY_HW_SPI0_SCK   (PA_8)   // J20 pin 28
#define MICROPY_HW_SPI0_MOSI  (PA_9)   // J20 pin 32
#define MICROPY_HW_SPI0_MISO  (PA_10)  // J20 pin 29
#define MICROPY_HW_SPI0_CS    (PA_11)  // J20 pin 31
// machine.I2CTarget(0/1) default pins:
#define MICROPY_HW_I2C0_SCL  (PA_16)   // J20 pin 35
#define MICROPY_HW_I2C0_SDA  (PA_17)   // J20 pin 33
#define MICROPY_HW_I2C1_SCL  (PB_6)    // J20 pin 5
#define MICROPY_HW_I2C1_SDA  (PB_7)    // J20 pin 7

#endif // MICROPY_INCLUDED_AMEBA_BOARDS_EV8711FLM_MPCONFIGBOARD_H
