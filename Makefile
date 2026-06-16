SHELL := /bin/bash

ROOT     := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PORT_DIR := $(ROOT)ports/ameba-rtos-m
ENV_SH   := $(ROOT)ameba-rtos/env.sh
AMEBA    := $(ROOT)ameba-rtos/ameba.py

# Target SoC, read from soc_info.json; switching chips needs no edit here.
DEVICE := $(shell python3 -c "import json;print(json.load(open('$(PORT_DIR)/soc_info.json'))['soc']['name'])")
# Retry on transient flash errors (b'\xe2' / DEV_TIMEOUT).
RETRY  ?= 3

# deploy requires an explicit serial port, e.g. make deploy PORT=/dev/ttyUSB0
ifneq ($(filter deploy,$(MAKECMDGOALS)),)
  ifeq ($(PORT),)
    $(error PORT is required, e.g. make deploy PORT=/dev/ttyUSB0)
  endif
endif

.PHONY: all build pristine clean deploy submodules

all: build

build: submodules
	cd $(PORT_DIR) && source $(ENV_SH) && python $(AMEBA) build

pristine: submodules
	cd $(PORT_DIR) && source $(ENV_SH) && python $(AMEBA) build -p

clean:
	cd $(PORT_DIR) && source $(ENV_SH) && python $(AMEBA) clean

# Flash only -- skips build so repeated deploys are fast.
# To build and flash in one go: make build deploy PORT=/dev/ttyUSB0
# Serial port is required: make deploy PORT=/dev/ttyUSB0 [BAUD=115200]
deploy:
	@cd $(PORT_DIR) && source $(ENV_SH) && for i in $$(seq 1 $(RETRY)); do \
	  echo ">> flash $$i/$(RETRY) ($(PORT) $(DEVICE))"; \
	  python $(AMEBA) flash -p $(PORT) $(if $(BAUD),-b $(BAUD)) -dev $(DEVICE) && exit 0; \
	done; echo ">> flash failed"; exit 1

# Sync submodules to the commits recorded by the parent repo. Idempotent:
# a no-op offline when already in sync, so running it on every build is cheap.
# Unlike a one-time stamp, it also picks up submodule pointer bumps after a
# `git pull`, avoiding a silent build against a stale SDK.
submodules:
	git -C $(ROOT) submodule update --init --depth 1 micropython ameba-rtos
	git -C $(ROOT)micropython submodule update --init --depth 1 lib/berkeley-db-1.xx lib/micropython-lib
