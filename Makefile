# bongOS top-level build. One Makefile including a per-component fragment per mk/*.mk
# (ARCHITECTURE §3). No autotools, no CMake.
SHELL := /bin/bash
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

BUILD := build
RELEASE ?= 0

include mk/branding.mk
include mk/format.mk
include mk/host-tests.mk
include mk/image.mk
include mk/qemu.mk
include mk/test.mk

.PHONY: all clean lint
all: image

lint: format-check
	@echo "lint: static analysis beyond formatting arrives with the first C sources"

clean:
	$(if $(strip $(BUILD)),,$(error BUILD is empty))
	rm -rf $(BUILD)
