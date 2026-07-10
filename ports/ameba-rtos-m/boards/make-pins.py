#!/usr/bin/env python3
# Generate the Pin.board locals-dict fragment for the ameba-rtos-m port from a
# board's pins.csv.
#
# The output is a C fragment that is #included inside the
# machine_pin_board_pins_locals_dict_table[] initializer in machine_pin.c, so
# each board's set of named pins lives in data (pins.csv) rather than in C.
#
# pins.csv format (one entry per non-blank, non-'#'-comment line):
#     board_name,cpu_name
# where board_name is the user-facing name (Pin.board.<board_name> /
# Pin("<board_name>")) and cpu_name is the SDK PinName enum symbol.
# For example:
#     PA13,PA_13
# emits:
#     { MP_ROM_QSTR(MP_QSTR_PA13), MP_ROM_PTR(&machine_pin_obj_table[PA_13]) },
#
# No SoC validation is done here on purpose: the emitted C references the SDK
# PinName enum symbol directly, so a name that is invalid for the compiled SoC
# (e.g. PC_0 on AmebaDplus) fails at compile time with a clear "undeclared"
# error rather than needing a duplicate valid-pin table in Python.

import argparse
import csv
import os


def main():
    ap = argparse.ArgumentParser(description="Generate Pin.board dict fragment from pins.csv")
    ap.add_argument("--csv", help="path to the board's pins.csv (optional)")
    ap.add_argument("--output", required=True, help="path to the C fragment to write")
    args = ap.parse_args()

    entries = []
    if args.csv and os.path.exists(args.csv):
        with open(args.csv, "r") as f:
            for row in csv.reader(f):
                if not row or row[0].strip().startswith("#"):
                    continue
                if len(row) < 2:
                    continue
                board = row[0].strip()
                cpu = row[1].strip()
                if not board or not cpu:
                    continue
                entries.append(
                    "    {{ MP_ROM_QSTR(MP_QSTR_{}), MP_ROM_PTR(&machine_pin_obj_table[{}]) }},".format(
                        board, cpu
                    )
                )

    out_dir = os.path.dirname(os.path.abspath(args.output))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.output, "w") as f:
        f.write("// Auto-generated from pins.csv by make-pins.py. Do not edit.\n")
        for line in entries:
            f.write(line + "\n")


if __name__ == "__main__":
    main()
