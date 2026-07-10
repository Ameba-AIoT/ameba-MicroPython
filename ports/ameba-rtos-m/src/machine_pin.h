// ports/ameba-rtos-m/src/machine_pin.h
#pragma once
#include "py/obj.h"

extern const mp_obj_type_t machine_pin_type;
extern const mp_obj_type_t machine_pin_irq_type;

// Populate machine_pin_obj_table for the compiled SoC's pin banks.  Must be
// called once during startup before any machine.Pin use.
void machine_pin_table_init(void);
