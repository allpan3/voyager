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
WAVEFORM_ARG := $(if $(WAVEFORM),--waveform,)

SYSTEMC_CC ?= $(CATAPULT_ROOT)/bin/g++
VERILATOR ?= verilator
VERILATOR_ROOT ?= $(shell $(VERILATOR) -V 2>/dev/null | sed -n 's/.*VERILATOR_ROOT *= *//p' | head -1)

SYSTEMC_BUILD := $(THIS_DIR)/build/systemc/$(DATATYPE)_$(IC_DIMENSION)x$(OC_DIMENSION)
SYSTEMC_GENERATED := $(SYSTEMC_BUILD)/generated
SYSTEMC_CASES := $(THIS_DIR)/systemc/cim_element_cosim_cases.py
SYSTEMC_CASES_INC := $(SYSTEMC_GENERATED)/CIMElementSystemCTestCases.inc
SYSTEMC_CASES_MK := $(SYSTEMC_GENERATED)/cim_element_cosim_cases.mk
SYSTEMC_RUNNER := $(THIS_DIR)/systemc/run_systemc_test.py
CIM_VERILATED_DIR := $(SYSTEMC_BUILD)/verilated
CIM_RTL_SRCS := \
	$(REPO_ROOT)/src/CIM/cim_typedefs.svh \
	$(REPO_ROOT)/src/CIM/cim_macro_wrapper.sv \
	$(REPO_ROOT)/src/CIM/cim_macro_model.sv \
	$(REPO_ROOT)/src/CIM/cim_macro_1.sv \
	$(REPO_ROOT)/src/CIM/cim_element.sv

-include $(SYSTEMC_CASES_MK)

SYSTEMC_CXXFLAGS := \
	-I$(CATAPULT_ROOT)/shared/include/ \
	-I$(REPO_ROOT)/lib/ \
	-I$(REPO_ROOT)/src/ \
	-I$(REPO_ROOT) \
	-I$(VERILATOR_ROOT)/include \
	-I$(SYSTEMC_GENERATED) \
	$(CIM_ELEMENT_VERILATED_INCLUDE_FLAGS) \
	-DSC_INCLUDE_DYNAMIC_PROCESSES \
	-Wno-unknown-pragmas \
	-Wno-unused-but-set-variable \
	-Wno-unused-variable \
	-Wno-sign-compare \
	-Wno-bool-operation \
	-Wno-maybe-uninitialized \
	-Wno-class-memaccess \
	-Wall \
	-Wno-bool-compare \
	-D$(DATATYPE) \
	-DIC_DIMENSION=$(IC_DIMENSION) \
	-DOC_DIMENSION=$(OC_DIMENSION) \
	-O3 \
	-std=c++17 \
	-Wno-deprecated-declarations
SYSTEMC_LDFLAGS := \
	-L$(CATAPULT_ROOT)/shared/lib/ \
	-lsystemc \
	-lpthread

.PHONY: test test-macro test-element test-array build-systemc build-systemc-element build-systemc-array test-systemc-element test-systemc-array check-systemc-env check-verilator-env clean

test: test-macro test-element test-systemc-element test-systemc-array

test-macro:
>$(PYTHON) $(THIS_DIR)/verilog/test_cim_macro.py $(JOBS_ARG) $(WAVEFORM_ARG) $(TEST_ARGS) $(MODEL_TEST_ARGS)

test-element:
>$(PYTHON) $(THIS_DIR)/verilog/test_cim_element.py $(JOBS_ARG) $(WAVEFORM_ARG) $(TEST_ARGS) $(ELEMENT_TEST_ARGS)

test-array: test-systemc-array

build-systemc: $(SYSTEMC_BUILD)/CIMElementSystemCTest $(SYSTEMC_BUILD)/CIMArraySystemCTest

build-systemc-element: $(SYSTEMC_BUILD)/CIMElementSystemCTest

build-systemc-array: $(SYSTEMC_BUILD)/CIMArraySystemCTest

test-systemc-element: check-systemc-env
>@$(PYTHON) $(SYSTEMC_RUNNER) --suite cim_element_systemc --build-target build-systemc-element --binary $(SYSTEMC_BUILD)/CIMElementSystemCTest --log-dir $(SYSTEMC_BUILD)/logs/cim_element --makefile $(THIS_DIR)/Makefile --datatype $(DATATYPE) --ic-dimension $(IC_DIMENSION) --oc-dimension $(OC_DIMENSION)

test-systemc-array: check-systemc-env
>@$(PYTHON) $(SYSTEMC_RUNNER) --suite cim_array_systemc --build-target build-systemc-array --binary $(SYSTEMC_BUILD)/CIMArraySystemCTest --log-dir $(SYSTEMC_BUILD)/logs/cim_array --makefile $(THIS_DIR)/Makefile --datatype $(DATATYPE) --ic-dimension $(IC_DIMENSION) --oc-dimension $(OC_DIMENSION)

$(SYSTEMC_BUILD)/CIMElementSystemCTest: $(SYSTEMC_BUILD)/CIMElementSystemCTest.o $(CIM_ELEMENT_VERILATED_LIBS)
>$(SYSTEMC_CC) -o $@ $< $(CIM_ELEMENT_VERILATED_LIBS) $(CIM_ELEMENT_VERILATED_RUNTIME_LIB) $(SYSTEMC_LDFLAGS)

$(SYSTEMC_BUILD)/CIMArraySystemCTest: $(SYSTEMC_BUILD)/CIMArraySystemCTest.o
>$(SYSTEMC_CC) -o $@ $^ $(SYSTEMC_LDFLAGS)

$(SYSTEMC_BUILD)/CIMElementSystemCTest.o: $(THIS_DIR)/CIMElementSystemCTest.cc $(REPO_ROOT)/src/CIMElement.h $(REPO_ROOT)/src/ArchitectureParams.h $(SYSTEMC_CASES_INC) $(CIM_ELEMENT_VERILATED_LIBS) | check-systemc-env check-verilator-env
>mkdir -p $(SYSTEMC_BUILD)
>$(SYSTEMC_CC) $(SYSTEMC_CXXFLAGS) -c -o $@ $<

$(SYSTEMC_BUILD)/CIMArraySystemCTest.o: $(THIS_DIR)/CIMArraySystemCTest.cc $(REPO_ROOT)/src/CIMArray.h $(REPO_ROOT)/src/CIMTile.h $(REPO_ROOT)/src/CIMElement.h $(REPO_ROOT)/src/ArchitectureParams.h | check-systemc-env
>mkdir -p $(SYSTEMC_BUILD)
>$(SYSTEMC_CC) $(SYSTEMC_CXXFLAGS) -c -o $@ $<

$(SYSTEMC_CASES_MK): $(SYSTEMC_CASES)
>mkdir -p $(SYSTEMC_GENERATED)
>$(PYTHON) $(SYSTEMC_CASES) --makefile $@

check-systemc-env:
>@test -n "$(DATATYPE)" || { echo "DATATYPE environment variable is not set"; exit 2; }
>@test -n "$(IC_DIMENSION)" || { echo "IC_DIMENSION environment variable is not set"; exit 2; }
>@test -n "$(OC_DIMENSION)" || { echo "OC_DIMENSION environment variable is not set"; exit 2; }
>@test -n "$(CATAPULT_ROOT)" || { echo "CATAPULT_ROOT environment variable is not set"; exit 2; }

check-verilator-env:
>@test -n "$(VERILATOR_ROOT)" || { echo "VERILATOR_ROOT could not be detected"; exit 2; }

clean:
>rm -rf $(THIS_DIR)/build
