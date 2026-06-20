# Make targets for CIM unittest suites
.RECIPEPREFIX := >
THIS_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(THIS_DIR)/../..)
CONDA_ENV ?= $(abspath $(REPO_ROOT)/../.conda-env)

ifneq ($(wildcard $(CONDA_ENV)/bin/python3),)
PYTHON ?= $(CONDA_ENV)/bin/python3
else
PYTHON ?= python3
endif

export PATH := $(CONDA_ENV)/bin:$(PATH)

TEST_ARGS ?=
MODEL_TEST_ARGS ?=
ELEMENT_TEST_ARGS ?=
JOBS_ARG := $(if $(JOBS),--jobs $(JOBS),)

.PHONY: test test-model test-unit clean

test: test-model test-unit

test-model:
>$(PYTHON) $(THIS_DIR)/test_cim_model.py $(JOBS_ARG) $(TEST_ARGS) $(MODEL_TEST_ARGS)

test-unit:
>$(PYTHON) $(THIS_DIR)/test_cim_unit.py $(JOBS_ARG) $(TEST_ARGS) $(UNIT_TEST_ARGS)

clean:
>rm -rf $(THIS_DIR)/build
