SHELL := /bin/bash

ROOT     := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PORT_DIR := $(ROOT)ports/ameba-rtos-m

# Board to build for. Override with: make BOARD=OTHER_BOARD
BOARD ?= PKE8721DAF

# deploy requires an explicit serial port, e.g. make deploy PORT=/dev/ttyUSB0
ifneq ($(filter deploy,$(MAKECMDGOALS)),)
  ifeq ($(PORT),)
    $(error PORT is required, e.g. make deploy PORT=/dev/ttyUSB0)
  endif
endif

.PHONY: all build pristine clean deploy

all: build

# Delegate all targets to the port Makefile, which handles board selection,
# soc_info.json sync, MICROPY_BOARD injection and makeimg.py packaging.
build:
	$(MAKE) -C $(PORT_DIR) BOARD=$(BOARD)

pristine:
	$(MAKE) -C $(PORT_DIR) BOARD=$(BOARD) pristine

clean:
	$(MAKE) -C $(PORT_DIR) BOARD=$(BOARD) clean

deploy:
	$(MAKE) -C $(PORT_DIR) BOARD=$(BOARD) deploy PORT=$(PORT) $(if $(BAUD),BAUD=$(BAUD))
