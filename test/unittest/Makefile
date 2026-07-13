# Make targets for CIM unittest suites
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

# Default SystemC-sim configuration (selects the -D flags and the build subdir
# DATATYPE_ICxOC). Override on the command line to sweep
DATATYPE ?= INT8
IC_DIMENSION ?= 4
OC_DIMENSION ?= 2

SYSTEMC_CC ?= $(CATAPULT_ROOT)/bin/g++
ifneq ($(wildcard $(CONDA_ENV)/bin/verilator),)
VERILATOR ?= $(CONDA_ENV)/bin/verilator
else
VERILATOR ?= verilator
endif
VERILATOR_ROOT ?= $(shell $(VERILATOR) -V 2>/dev/null | sed -n 's/.*VERILATOR_ROOT *= *//p' | head -1)

SYSTEMC_BUILD := $(THIS_DIR)/build/systemc/$(DATATYPE)_$(IC_DIMENSION)x$(OC_DIMENSION)
CIM_PROCESSOR_IC_DIMENSION := 8
CIM_PROCESSOR_OC_DIMENSION := 3
CIM_PROCESSOR_SYSTEMC_BUILD := $(THIS_DIR)/build/systemc/INT8_$(CIM_PROCESSOR_IC_DIMENSION)x$(CIM_PROCESSOR_OC_DIMENSION)
SYSTEMC_GENERATED := $(SYSTEMC_BUILD)/generated
SYSTEMC_CASES := $(THIS_DIR)/systemc/cim_element_cosim_cases.py
SYSTEMC_CASES_INC := $(SYSTEMC_GENERATED)/CIMElementTbCases.inc
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

###########################################################
# Standalone Catapult HLS for CIMTile and CIMArray
#
# Synthesizes either enclosing block in isolation.
# Override any CIM_* / CLOCK_PERIOD / TECHNOLOGY
# on the command line, e.g.:
#   make hls-array CLOCK_PERIOD=1.25 CIM_CH_IN=64
###########################################################
CATAPULT ?= catapult
HLS_DRIVER := $(THIS_DIR)/hls.tcl

CLOCK_PERIOD ?= 1.0
TECHNOLOGY ?= generic
HLS_BUILD := $(THIS_DIR)/build/catapult/$(TECHNOLOGY)/clock_$(CLOCK_PERIOD)

# Geometry pinned by CIMArrayTb.cc in SCVerify mode
SCVERIFY_ARRAY_HLS_ARGS := \
	CIM_CH_IN=4 CIM_CH_OUT=2 CIM_B_SETS=2 \
	CIM_BASE_A_WIDTH=4 CIM_BASE_B_WIDTH=4 CIM_BASE_C_WIDTH=12 \
	CIM_WRITE_CH_IN=2 CIM_MAC_LATENCY=2 CIM_MODE=0 \
	CIM_A_WIDTH=4 CIM_B_WIDTH=4 CIM_SIGNED=false \
	CIM_TILE_INPUT_LANES=2 CIM_TILE_OUTPUT_LANES=2 \
	CIM_REDUCTION_GROUPS=2 CIM_MULTICAST_GROUPS=3 \
	DATATYPE=INT8 IC_DIMENSION=4 OC_DIMENSION=2

# Geometry pinned by CIMProcessorTb.cc in SCVerify mode
SCVERIFY_PROCESSOR_HLS_ARGS := \
	CIM_CH_IN=2 CIM_CH_OUT=2 CIM_B_SETS=2 \
	CIM_BASE_A_WIDTH=4 CIM_BASE_B_WIDTH=4 CIM_BASE_C_WIDTH=12 \
	CIM_WRITE_CH_IN=1 CIM_MAC_LATENCY=1 CIM_MODE=0 \
	CIM_A_WIDTH=8 CIM_B_WIDTH=8 CIM_SIGNED=true \
	CIM_TILE_INPUT_LANES=2 CIM_TILE_OUTPUT_LANES=1 \
	CIM_REDUCTION_GROUPS=2 CIM_MULTICAST_GROUPS=3 \
	CIM_C_PORT_ORIENTATION=1 DATATYPE=INT8 \
	IC_DIMENSION=$(CIM_PROCESSOR_IC_DIMENSION) \
	OC_DIMENSION=$(CIM_PROCESSOR_OC_DIMENSION)

# CIM template parameters. Defaults match the production macro geometry in
# scripts/architecture.tcl; override individually to sweep configurations.
CIM_CH_IN ?= 64
CIM_CH_OUT ?= 8
CIM_B_SETS ?= 18
CIM_BASE_A_WIDTH ?= 4
CIM_BASE_B_WIDTH ?= 4
CIM_BASE_C_WIDTH ?= 20
CIM_WRITE_CH_IN ?= 1
CIM_MAC_LATENCY ?= 1
CIM_MODE ?= 0
CIM_A_WIDTH ?= 8
CIM_B_WIDTH ?= 8
CIM_SIGNED ?= true

# Shared tile geometry plus CIMArray tile-grid dimensions
CIM_TILE_INPUT_LANES ?= 1
CIM_TILE_OUTPUT_LANES ?= 4
CIM_REDUCTION_GROUPS ?= 1
CIM_MULTICAST_GROUPS ?= 1

# Transfer ports, in tiles per transfer. Each is pinned to "one beat per
# transfer" today: A_PORT_TILES to REDUCTION_GROUPS (a full A column),
# B_PORT_TILES to MULTICAST_GROUPS (a full B write-row). C_PORT_TILES follows
# the compile-time orientation: reduction groups for 0, multicast groups for 1
CIM_A_PORT_TILES := $(CIM_REDUCTION_GROUPS)
CIM_B_PORT_TILES := $(CIM_MULTICAST_GROUPS)
CIM_C_PORT_ORIENTATION ?= 0
ifeq ($(filter $(CIM_C_PORT_ORIENTATION),0 1),)
$(error CIM_C_PORT_ORIENTATION must be 0 or 1)
endif
CIM_C_PORT_TILES := $(if $(filter 0,$(CIM_C_PORT_ORIENTATION)),$(CIM_REDUCTION_GROUPS),$(CIM_MULTICAST_GROUPS))

# Base macro geometry arguments followed by tile and array-grid geometry
# Spelling (comma-space, `true`) must match how the compiler mangles the
# specialization
CIM_TPARAMS := $(CIM_CH_IN), $(CIM_CH_OUT), $(CIM_B_SETS), $(CIM_BASE_A_WIDTH), $(CIM_BASE_B_WIDTH), $(CIM_BASE_C_WIDTH), $(CIM_WRITE_CH_IN), $(CIM_MAC_LATENCY), $(CIM_MODE), $(CIM_A_WIDTH), $(CIM_B_WIDTH), $(CIM_SIGNED)
CIM_TILE_TPARAMS := $(CIM_TPARAMS), $(CIM_TILE_INPUT_LANES), $(CIM_TILE_OUTPUT_LANES)
CIM_ARRAY_TPARAMS := $(CIM_TPARAMS), $(CIM_TILE_INPUT_LANES), $(CIM_TILE_OUTPUT_LANES), $(CIM_REDUCTION_GROUPS), $(CIM_MULTICAST_GROUPS), $(CIM_A_PORT_TILES), $(CIM_B_PORT_TILES), $(CIM_C_PORT_TILES), $(CIM_C_PORT_ORIENTATION)
HLS_TOP_TILE := CIMTile<$(CIM_TILE_TPARAMS)>
HLS_TOP_ARRAY := CIMArray<$(CIM_ARRAY_TPARAMS)>
CIM_PROCESSOR_BUFFER_SIZE ?= 16
CIM_PROCESSOR_TPARAMS := InputTypeList, WeightTypeList, DataTypes::int8, DataTypes::int8, DataTypes::int24, DataTypes::int24, DataTypes::fp8_e8m0, $(CIM_PROCESSOR_IC_DIMENSION), $(CIM_PROCESSOR_OC_DIMENSION), $(CIM_PROCESSOR_BUFFER_SIZE), $(CIM_ARRAY_TPARAMS)
HLS_TOP_PROCESSOR := CIMProcessor<$(CIM_PROCESSOR_TPARAMS)>
CIM_TILE_CONFIG := ci$(CIM_CH_IN)_co$(CIM_CH_OUT)_bs$(CIM_B_SETS)_base$(CIM_BASE_A_WIDTH)x$(CIM_BASE_B_WIDTH)x$(CIM_BASE_C_WIDTH)_w$(CIM_WRITE_CH_IN)_lat$(CIM_MAC_LATENCY)_m$(CIM_MODE)_a$(CIM_A_WIDTH)_b$(CIM_B_WIDTH)_s$(CIM_SIGNED)_til$(CIM_TILE_INPUT_LANES)_tol$(CIM_TILE_OUTPUT_LANES)
CIM_ARRAY_CONFIG := ci$(CIM_CH_IN)_co$(CIM_CH_OUT)_bs$(CIM_B_SETS)_base$(CIM_BASE_A_WIDTH)x$(CIM_BASE_B_WIDTH)x$(CIM_BASE_C_WIDTH)_w$(CIM_WRITE_CH_IN)_lat$(CIM_MAC_LATENCY)_m$(CIM_MODE)_a$(CIM_A_WIDTH)_b$(CIM_B_WIDTH)_s$(CIM_SIGNED)_til$(CIM_TILE_INPUT_LANES)_tol$(CIM_TILE_OUTPUT_LANES)_rg$(CIM_REDUCTION_GROUPS)_mg$(CIM_MULTICAST_GROUPS)_cpo$(CIM_C_PORT_ORIENTATION)
HLS_TILE_BUILD := $(HLS_BUILD)/CIMTile/$(CIM_TILE_CONFIG)
HLS_TILE_PROJECT := $(HLS_TILE_BUILD)/CIMTile.v1
HLS_TILE_RTL := $(HLS_TILE_PROJECT)/rtl.v
HLS_ARRAY_BUILD := $(HLS_BUILD)/CIMArray/$(CIM_ARRAY_CONFIG)
HLS_ARRAY_PROJECT := $(HLS_ARRAY_BUILD)/CIMArray.v1
HLS_ARRAY_RTL := $(HLS_ARRAY_PROJECT)/rtl.v
HLS_PROCESSOR_BUILD := $(HLS_BUILD)/CIMProcessor/$(CIM_ARRAY_CONFIG)_ic$(CIM_PROCESSOR_IC_DIMENSION)_oc$(CIM_PROCESSOR_OC_DIMENSION)
HLS_PROCESSOR_PROJECT := $(HLS_PROCESSOR_BUILD)/CIMProcessor.v1
HLS_PROCESSOR_RTL := $(HLS_PROCESSOR_PROJECT)/rtl.v
SCVERIFY_ARRAY_PROJECT := $(HLS_ARRAY_PROJECT)
SCVERIFY_ARRAY_MK := $(SCVERIFY_ARRAY_PROJECT)/scverify/Verify_rtl_v_vcs.mk
SCVERIFY_ARRAY_TB := $(THIS_DIR)/systemc/CIMArrayTb.cc
SCVERIFY_PROCESSOR_PROJECT := $(HLS_PROCESSOR_PROJECT)
SCVERIFY_PROCESSOR_MK := $(SCVERIFY_PROCESSOR_PROJECT)/scverify/Verify_rtl_v_vcs.mk
SCVERIFY_PROCESSOR_TB := $(THIS_DIR)/systemc/CIMProcessorTb.cc

# Shared environment forwarded into the driver
HLS_TILE_DEPS := \
	$(THIS_DIR)/Makefile \
	$(HLS_DRIVER) \
	$(REPO_ROOT)/src/CIMTile.h \
	$(REPO_ROOT)/src/CIMElement.h
HLS_ARRAY_DEPS := \
	$(THIS_DIR)/Makefile \
	$(HLS_DRIVER) \
	$(REPO_ROOT)/src/CIMArray.h \
	$(REPO_ROOT)/src/CIMTile.h \
	$(REPO_ROOT)/src/CIMElement.h
HLS_PROCESSOR_DEPS := \
	$(THIS_DIR)/Makefile \
	$(HLS_DRIVER) \
	$(REPO_ROOT)/src/CIMProcessor.h \
	$(REPO_ROOT)/src/CIMArray.h \
	$(REPO_ROOT)/src/CIMTile.h \
	$(REPO_ROOT)/src/CIMElement.h
SCVERIFY_ARRAY_DEPS := \
	$(HLS_ARRAY_DEPS) \
	$(SCVERIFY_ARRAY_TB)
SCVERIFY_PROCESSOR_DEPS := \
	$(HLS_PROCESSOR_DEPS) \
	$(SCVERIFY_PROCESSOR_TB)

HLS_ENV = \
	REPO_ROOT=$(REPO_ROOT) \
	CLOCK_PERIOD=$(CLOCK_PERIOD) \
	TECHNOLOGY=$(TECHNOLOGY) \
	DATATYPE=$(DATATYPE) \
	IC_DIMENSION=$(IC_DIMENSION) \
	OC_DIMENSION=$(OC_DIMENSION) \
	CIM_A_PORT_TILES=$(CIM_A_PORT_TILES) \
	CIM_C_PORT_ORIENTATION=$(CIM_C_PORT_ORIENTATION) \
	HLS_RESET_NAME=rstn \
	$(if $(HLS_SCVERIFY),HLS_SCVERIFY=$(HLS_SCVERIFY))

.PHONY: test test-macro test-element build-systemc build-systemc-element build-systemc-tile build-systemc-array build-systemc-processor test-systemc-element test-systemc-tile test-systemc-array test-systemc-processor check-systemc-env check-verilator-env clean hls hls-tile hls-array hls-processor hls-processor-run scverify-array scverify-array-run scverify-processor scverify-processor-run check-catapult-root check-catapult-env FORCE

test: test-macro test-element test-systemc-element test-systemc-tile test-systemc-array test-systemc-processor

test-macro:
	$(PYTHON) $(THIS_DIR)/verilog/test_cim_macro.py $(JOBS_ARG) $(WAVEFORM_ARG) $(TEST_ARGS) $(MODEL_TEST_ARGS)

test-element:
	$(PYTHON) $(THIS_DIR)/verilog/test_cim_element.py $(JOBS_ARG) $(WAVEFORM_ARG) $(TEST_ARGS) $(ELEMENT_TEST_ARGS)

build-systemc: $(SYSTEMC_BUILD)/CIMElementTb $(SYSTEMC_BUILD)/CIMTileTb $(SYSTEMC_BUILD)/CIMArrayTb

build-systemc-element: $(SYSTEMC_BUILD)/CIMElementTb

build-systemc-tile: $(SYSTEMC_BUILD)/CIMTileTb

build-systemc-array: $(SYSTEMC_BUILD)/CIMArrayTb

build-systemc-processor: $(CIM_PROCESSOR_SYSTEMC_BUILD)/CIMProcessorTb

test-systemc-element: check-systemc-env
	@$(PYTHON) $(SYSTEMC_RUNNER) --suite cim_element_systemc --build-target build-systemc-element --binary $(SYSTEMC_BUILD)/CIMElementTb --log-dir $(SYSTEMC_BUILD)/logs/cim_element --makefile $(THIS_DIR)/Makefile --datatype $(DATATYPE) --ic-dimension $(IC_DIMENSION) --oc-dimension $(OC_DIMENSION)

test-systemc-tile: check-systemc-env
	@$(PYTHON) $(SYSTEMC_RUNNER) --suite cim_tile_systemc --build-target build-systemc-tile --binary $(SYSTEMC_BUILD)/CIMTileTb --log-dir $(SYSTEMC_BUILD)/logs/cim_tile --makefile $(THIS_DIR)/Makefile --datatype $(DATATYPE) --ic-dimension $(IC_DIMENSION) --oc-dimension $(OC_DIMENSION)

test-systemc-array: check-systemc-env
	@$(PYTHON) $(SYSTEMC_RUNNER) --suite cim_array_systemc --build-target build-systemc-array --binary $(SYSTEMC_BUILD)/CIMArrayTb --log-dir $(SYSTEMC_BUILD)/logs/cim_array --makefile $(THIS_DIR)/Makefile --datatype $(DATATYPE) --ic-dimension $(IC_DIMENSION) --oc-dimension $(OC_DIMENSION)

test-systemc-processor: check-systemc-env
	@$(PYTHON) $(SYSTEMC_RUNNER) --suite cim_processor_systemc --build-target build-systemc-processor --binary $(CIM_PROCESSOR_SYSTEMC_BUILD)/CIMProcessorTb --log-dir $(CIM_PROCESSOR_SYSTEMC_BUILD)/logs/cim_processor --makefile $(THIS_DIR)/Makefile --datatype INT8 --ic-dimension $(CIM_PROCESSOR_IC_DIMENSION) --oc-dimension $(CIM_PROCESSOR_OC_DIMENSION)

$(SYSTEMC_BUILD)/CIMElementTb: $(SYSTEMC_BUILD)/CIMElementTb.o $(CIM_ELEMENT_VERILATED_LIBS)
	$(SYSTEMC_CC) -o $@ $< $(CIM_ELEMENT_VERILATED_LIBS) $(CIM_ELEMENT_VERILATED_RUNTIME_LIB) $(SYSTEMC_LDFLAGS)

$(SYSTEMC_BUILD)/CIMTileTb: $(SYSTEMC_BUILD)/CIMTileTb.o
	$(SYSTEMC_CC) -o $@ $^ $(SYSTEMC_LDFLAGS)

$(SYSTEMC_BUILD)/CIMArrayTb: $(SYSTEMC_BUILD)/CIMArrayTb.o
	$(SYSTEMC_CC) -o $@ $^ $(SYSTEMC_LDFLAGS)

$(CIM_PROCESSOR_SYSTEMC_BUILD)/CIMProcessorTb: $(CIM_PROCESSOR_SYSTEMC_BUILD)/CIMProcessorTb.o
	$(SYSTEMC_CC) -o $@ $^ $(SYSTEMC_LDFLAGS)

$(SYSTEMC_BUILD)/CIMElementTb.o: $(THIS_DIR)/systemc/CIMElementTb.cc $(REPO_ROOT)/src/CIMElement.h $(REPO_ROOT)/src/ArchitectureParams.h $(SYSTEMC_CASES_INC) $(CIM_ELEMENT_VERILATED_LIBS) | check-systemc-env check-verilator-env
	mkdir -p $(SYSTEMC_BUILD)
	$(SYSTEMC_CC) $(SYSTEMC_CXXFLAGS) -c -o $@ $<

$(SYSTEMC_BUILD)/CIMTileTb.o: $(THIS_DIR)/systemc/CIMTileTb.cc $(REPO_ROOT)/src/CIMTile.h $(REPO_ROOT)/src/CIMElement.h $(REPO_ROOT)/src/ArchitectureParams.h | check-systemc-env
	mkdir -p $(SYSTEMC_BUILD)
	$(SYSTEMC_CC) $(SYSTEMC_CXXFLAGS) -c -o $@ $<

$(SYSTEMC_BUILD)/CIMArrayTb.o: $(THIS_DIR)/systemc/CIMArrayTb.cc $(REPO_ROOT)/src/CIMArray.h $(REPO_ROOT)/src/CIMTile.h $(REPO_ROOT)/src/CIMElement.h $(REPO_ROOT)/src/ArchitectureParams.h | check-systemc-env
	mkdir -p $(SYSTEMC_BUILD)
	$(SYSTEMC_CC) $(SYSTEMC_CXXFLAGS) -c -o $@ $<

$(CIM_PROCESSOR_SYSTEMC_BUILD)/CIMProcessorTb.o: $(THIS_DIR)/systemc/CIMProcessorTb.cc $(REPO_ROOT)/src/CIMProcessor.h $(REPO_ROOT)/src/CIMArray.h $(REPO_ROOT)/src/CIMTile.h $(REPO_ROOT)/src/CIMElement.h $(REPO_ROOT)/src/ArchitectureParams.h | check-systemc-env
	mkdir -p $(CIM_PROCESSOR_SYSTEMC_BUILD)
	$(SYSTEMC_CC) $(SYSTEMC_CXXFLAGS) -c -o $@ $<

$(SYSTEMC_CASES_MK): $(SYSTEMC_CASES)
	mkdir -p $(SYSTEMC_GENERATED)
	$(PYTHON) $(SYSTEMC_CASES) --makefile $@

# CATAPULT_ROOT locates the toolchain shared by both flows: the SystemC/AC/
# Connections headers and g++ for the cosim build, and the HLS tool for synthesis.
check-catapult-root:
	@test -n "$(CATAPULT_ROOT)" || { echo "CATAPULT_ROOT is not set (source env.sh)"; exit 2; }

# SystemC cosim env: test-config vars plus the shared toolchain root (no HLS tool).
check-systemc-env: check-catapult-root
	@test -n "$(DATATYPE)" || { echo "DATATYPE environment variable is not set"; exit 2; }
	@test -n "$(IC_DIMENSION)" || { echo "IC_DIMENSION environment variable is not set"; exit 2; }
	@test -n "$(OC_DIMENSION)" || { echo "OC_DIMENSION environment variable is not set"; exit 2; }

check-verilator-env:
	@test -n "$(VERILATOR_ROOT)" || { echo "VERILATOR_ROOT could not be detected"; exit 2; }

# --- Catapult HLS ------------------------------------------------------------
hls: hls-tile hls-array hls-processor

hls-tile: $(HLS_TILE_RTL)

$(HLS_TILE_RTL): $(HLS_TILE_DEPS) | check-catapult-env
	mkdir -p $(HLS_TILE_BUILD)
	$(HLS_ENV) BLOCK=CIMTile HLS_TOP='$(HLS_TOP_TILE)' \
	DESIGN_HEADER=$(REPO_ROOT)/src/CIMTile.h \
	HLS_PROJECT_DIR=$(HLS_TILE_BUILD) HLS_CLOCKS='wclk mclk' \
	$(CATAPULT) -shell -file $(HLS_DRIVER) -logfile $(HLS_TILE_BUILD).log

hls-array: $(HLS_ARRAY_RTL)

$(HLS_ARRAY_RTL): $(HLS_ARRAY_DEPS) | check-catapult-env
	mkdir -p $(HLS_ARRAY_BUILD)
	$(HLS_ENV) BLOCK=CIMArray HLS_TOP='$(HLS_TOP_ARRAY)' \
	DESIGN_HEADER=$(REPO_ROOT)/src/CIMArray.h \
	HLS_PROJECT_DIR=$(HLS_ARRAY_BUILD) HLS_CLOCKS='clk' \
	$(CATAPULT) -shell -file $(HLS_DRIVER) -logfile $(HLS_ARRAY_BUILD).log

hls-processor:
	$(MAKE) -C $(THIS_DIR) hls-processor-run $(SCVERIFY_PROCESSOR_HLS_ARGS)

hls-processor-run: $(HLS_PROCESSOR_RTL)

$(HLS_PROCESSOR_RTL): $(HLS_PROCESSOR_DEPS) | check-catapult-env
	mkdir -p $(HLS_PROCESSOR_BUILD)
	$(HLS_ENV) CIM_C_PORT_ORIENTATION=1 BLOCK=CIMProcessor \
	HLS_TOP='$(HLS_TOP_PROCESSOR)' \
	HLS_CIM_ARRAY_TOP='$(HLS_TOP_ARRAY)' \
	DESIGN_HEADER=$(REPO_ROOT)/src/CIMProcessor.h \
	HLS_PROJECT_DIR=$(HLS_PROCESSOR_BUILD) HLS_CLOCKS='clk' \
	$(CATAPULT) -shell -file $(HLS_DRIVER) -logfile $(HLS_PROCESSOR_BUILD).log

scverify-array:
	$(MAKE) -C $(THIS_DIR) scverify-array-run HLS_SCVERIFY=1 HLS_SCVERIFY_TB=$(SCVERIFY_ARRAY_TB) $(SCVERIFY_ARRAY_HLS_ARGS)

scverify-array-run: $(SCVERIFY_ARRAY_MK)
	$(MAKE) -C $(SCVERIFY_ARRAY_PROJECT) -f ./scverify/Verify_rtl_v_vcs.mk SIMTOOL=vcs build
	$(MAKE) -C $(SCVERIFY_ARRAY_PROJECT) -f ./scverify/Verify_rtl_v_vcs.mk SIMTOOL=vcs sim

$(SCVERIFY_ARRAY_MK): $(SCVERIFY_ARRAY_DEPS) | check-catapult-env
	mkdir -p $(HLS_ARRAY_BUILD)
	$(HLS_ENV) HLS_SCVERIFY_TB=$(SCVERIFY_ARRAY_TB) \
	BLOCK=CIMArray HLS_TOP='$(HLS_TOP_ARRAY)' \
	DESIGN_HEADER=$(REPO_ROOT)/src/CIMArray.h \
	HLS_PROJECT_DIR=$(HLS_ARRAY_BUILD) HLS_CLOCKS='clk' \
	$(CATAPULT) -shell -file $(HLS_DRIVER) -logfile $(HLS_ARRAY_BUILD).log

scverify-processor:
	$(MAKE) -C $(THIS_DIR) scverify-processor-run HLS_SCVERIFY=1 HLS_SCVERIFY_TB=$(SCVERIFY_PROCESSOR_TB) $(SCVERIFY_PROCESSOR_HLS_ARGS)

scverify-processor-run: $(SCVERIFY_PROCESSOR_MK)
	$(MAKE) -C $(SCVERIFY_PROCESSOR_PROJECT) -f ./scverify/Verify_rtl_v_vcs.mk SIMTOOL=vcs build
	$(MAKE) -C $(SCVERIFY_PROCESSOR_PROJECT) -f ./scverify/Verify_rtl_v_vcs.mk SIMTOOL=vcs sim

$(SCVERIFY_PROCESSOR_MK): $(SCVERIFY_PROCESSOR_DEPS) | check-catapult-env
	mkdir -p $(HLS_PROCESSOR_BUILD)
	$(HLS_ENV) CIM_C_PORT_ORIENTATION=1 \
	HLS_SCVERIFY_TB=$(SCVERIFY_PROCESSOR_TB) BLOCK=CIMProcessor \
	HLS_TOP='$(HLS_TOP_PROCESSOR)' \
	HLS_CIM_ARRAY_TOP='$(HLS_TOP_ARRAY)' \
	DESIGN_HEADER=$(REPO_ROOT)/src/CIMProcessor.h \
	HLS_PROJECT_DIR=$(HLS_PROCESSOR_BUILD) HLS_CLOCKS='clk' \
	$(CATAPULT) -shell -file $(HLS_DRIVER) -logfile $(HLS_PROCESSOR_BUILD).log

# HLS env: the shared toolchain root plus the catapult binary (no test-config vars).
check-catapult-env: check-catapult-root
	@command -v $(CATAPULT) >/dev/null 2>&1 || { echo "'$(CATAPULT)' not found in PATH (source env.sh)"; exit 2; }

clean:
	rm -rf $(THIS_DIR)/build
