#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk


APP = nvmf_tokenize

C_SRCS = nvmf_main.c nvmf_handler.c nvmf_llama.c
CXX_SRCS = do_rag.cpp

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_nvmf

CFLAGS += $(shell pkg-config --cflags llama)
LIBS += $(shell pkg-config --libs llama) -lggml-cpu # workarounds for incomplete .pc

CXXFLAGS += $(shell pkg-config --cflags libpq)
CFLAGS += $(shell pkg-config --cflags libpq)
LIBS += $(shell pkg-config --libs libpq)
LIBS += -lstdc++

ifeq ($(SPDK_ROOT_DIR)/lib/env_dpdk,$(CONFIG_ENV))
SPDK_LIB_LIST += env_dpdk_rpc
endif

ifeq ($(OS),Linux)
SPDK_LIB_LIST += event_nbd
endif

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk

install: $(APP)
	$(INSTALL_APP)

uninstall:
	$(UNINSTALL_APP)
