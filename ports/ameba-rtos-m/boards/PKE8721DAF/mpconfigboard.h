#ifndef MICROPY_INCLUDED_AMEBA_BOARDS_PKE8721DAF_MPCONFIGBOARD_H
#define MICROPY_INCLUDED_AMEBA_BOARDS_PKE8721DAF_MPCONFIGBOARD_H

#define MICROPY_HW_BOARD_NAME   "PKE8721DAF"
#define MICROPY_HW_MCU_NAME     "RTL8721DAF"

// RGB LED: controlled by PA14 / PA15 / PA16
#define MICROPY_HW_LED_RED      (14)
#define MICROPY_HW_LED_GREEN    (15)
#define MICROPY_HW_LED_BLUE     (16)

// Default peripheral pins.  Given explicitly here (rather than relying on the
// #ifndef fallbacks in machine_uart.c / machine_spi.c / machine_i2c_target.c)
// so each board owns its own defaults.  Values match this board's headers.
// machine.UART(0/1) default TX/RX:
#define MICROPY_HW_UART0_TX  (PA_31)
#define MICROPY_HW_UART0_RX  (PA_30)
#define MICROPY_HW_UART1_TX  (PA_29)
#define MICROPY_HW_UART1_RX  (PA_28)
// machine.SPI(0) default pins:
#define MICROPY_HW_SPI0_SCK   (PA_26)
#define MICROPY_HW_SPI0_MOSI  (PA_27)
#define MICROPY_HW_SPI0_MISO  (PA_28)
#define MICROPY_HW_SPI0_CS    (PA_12)
// machine.I2CTarget(0/1) default pins (board header pin 14=SCL/13=SDA, 12/11):
#define MICROPY_HW_I2C0_SCL  (PA_26)
#define MICROPY_HW_I2C0_SDA  (PA_27)
#define MICROPY_HW_I2C1_SCL  (PA_28)
#define MICROPY_HW_I2C1_SDA  (PA_29)

#endif // MICROPY_INCLUDED_AMEBA_BOARDS_PKE8721DAF_MPCONFIGBOARD_H
