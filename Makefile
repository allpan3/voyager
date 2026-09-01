.DEFAULT_GOAL := TestRunner

export PROJ_ROOT = $(shell pwd)

# Compilers are different on different machines
CC := $(CATAPULT_ROOT)/bin/g++

export CODEGEN_DIR ?= test/compiler

# L2 port widths in bits, one word per cycle per port
# Left empty, ArchitectureParams.h derives them from the array dimensions, which
# makes external bandwidth scale with the array; set them to hold the L2
# interface constant across design points
export IC_PORT_WIDTH ?=
export OC_PORT_WIDTH ?=

export MATRIX_BACKEND ?= 0
CODEGEN_BACKEND = $(if $(filter 1,$(MATRIX_BACKEND)),cim,sa)
CODEGEN_BACKEND_GEOMETRY = $(CODEGEN_BACKEND)_$(IC_DIMENSION)x$(OC_DIMENSION)

export CIM_CH_IN ?= 64
export CIM_CH_OUT ?= 8
export CIM_B_SETS ?= 18
export CIM_BASE_A_WIDTH ?= 4
export CIM_BASE_B_WIDTH ?= 4
export CIM_BASE_C_WIDTH ?= 20
export CIM_WRITE_CH_IN ?= 1
export CIM_MAC_LATENCY ?= 1
export CIM_MODE ?= 0
export CIM_TILE_INPUT_AXIS_ELEMENTS ?= 1
export CIM_TILE_OUTPUT_AXIS_ELEMENTS ?= 4
export CIM_INPUT_AXIS_TILES ?= 1
export CIM_OUTPUT_AXIS_TILES ?= 1
export CIM_C_BEAT_LAYOUT ?= $(if $(filter 1,$(MATRIX_BACKEND)),1,0)
export CIM_A_PORT_TILES ?= $(CIM_INPUT_AXIS_TILES)
export CIM_B_PORT_TILES ?= $(CIM_OUTPUT_AXIS_TILES)
export CIM_C_PORT_TILES ?= $(if $(filter 0,$(CIM_C_BEAT_LAYOUT)),$(CIM_INPUT_AXIS_TILES),$(CIM_OUTPUT_AXIS_TILES))
export CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE ?= $(CIM_INPUT_AXIS_TILES)
export CIM_LOCAL_ACCUM_CONTEXTS ?= 4
export ENABLE_PERF_COUNTERS ?= 1

# Check if the environment variable is set
check_env_var:
ifndef DATATYPE
	$(error DATATYPE environment variables are not set)
endif
ifndef IC_DIMENSION
	$(error IC_DIMENSION environment variables are not set)
endif
ifndef OC_DIMENSION
	$(error OC_DIMENSION environment variables are not set)
endif

INC := \
	-I$(CATAPULT_ROOT)/shared/include/ \
	-Ilib/ \
	-Ilib/xtensor/include \
	-Ilib/xtl/include \
	-Ilib/spdlog/include \
	-Isrc/ \
	-I$(CONDA_PREFIX)/include \
	-I.

# TODO(fpedd): Fix code and remove Wno-* flags step by step
override BASE_FLAGS += \
	$(INC) \
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
	-DSPDLOG_COMPILED_LIB \
	-DSPDLOG_EOL=\"\" \
	-D$(DATATYPE) \
	-DIC_DIMENSION=$(IC_DIMENSION) \
	-DOC_DIMENSION=$(OC_DIMENSION) \
	-DMATRIX_BACKEND=$(MATRIX_BACKEND) \
	-DCIM_CH_IN=$(CIM_CH_IN) \
	-DCIM_CH_OUT=$(CIM_CH_OUT) \
	-DCIM_B_SETS=$(CIM_B_SETS) \
	-DCIM_BASE_A_WIDTH=$(CIM_BASE_A_WIDTH) \
	-DCIM_BASE_B_WIDTH=$(CIM_BASE_B_WIDTH) \
	-DCIM_BASE_C_WIDTH=$(CIM_BASE_C_WIDTH) \
	-DCIM_WRITE_CH_IN=$(CIM_WRITE_CH_IN) \
	-DCIM_MAC_LATENCY=$(CIM_MAC_LATENCY) \
	-DCIM_MODE=$(CIM_MODE) \
	-DCIM_TILE_INPUT_AXIS_ELEMENTS=$(CIM_TILE_INPUT_AXIS_ELEMENTS) \
	-DCIM_TILE_OUTPUT_AXIS_ELEMENTS=$(CIM_TILE_OUTPUT_AXIS_ELEMENTS) \
	-DCIM_INPUT_AXIS_TILES=$(CIM_INPUT_AXIS_TILES) \
	-DCIM_OUTPUT_AXIS_TILES=$(CIM_OUTPUT_AXIS_TILES) \
	-DCIM_A_PORT_TILES=$(CIM_A_PORT_TILES) \
	-DCIM_B_PORT_TILES=$(CIM_B_PORT_TILES) \
	-DCIM_C_PORT_TILES=$(CIM_C_PORT_TILES) \
	-DCIM_C_BEAT_LAYOUT=$(CIM_C_BEAT_LAYOUT) \
	-DCIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE=$(CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE) \
	-DCIM_LOCAL_ACCUM_CONTEXTS=$(CIM_LOCAL_ACCUM_CONTEXTS) \
	-DENABLE_PERF_COUNTERS=$(ENABLE_PERF_COUNTERS) \
	$(if $(IC_PORT_WIDTH),-DIC_PORT_WIDTH=$(IC_PORT_WIDTH)) \
	$(if $(OC_PORT_WIDTH),-DOC_PORT_WIDTH=$(OC_PORT_WIDTH))

ifndef INPUT_BUFFER_SIZE
	export INPUT_BUFFER_SIZE = 1024
else
	override BASE_FLAGS += -DINPUT_BUFFER_SIZE=$(INPUT_BUFFER_SIZE)
endif

ifndef WEIGHT_BUFFER_SIZE
	export WEIGHT_BUFFER_SIZE = 1024
else
	override BASE_FLAGS += -DWEIGHT_BUFFER_SIZE=$(WEIGHT_BUFFER_SIZE)
endif

ifndef ACCUM_BUFFER_SIZE
	export ACCUM_BUFFER_SIZE = 1024
else
	override BASE_FLAGS += -DACCUM_BUFFER_SIZE=$(ACCUM_BUFFER_SIZE)
endif

ifndef DOUBLE_BUFFERED_ACCUM_BUFFER
	export DOUBLE_BUFFERED_ACCUM_BUFFER = false
else
	override BASE_FLAGS += -DDOUBLE_BUFFERED_ACCUM_BUFFER=$(DOUBLE_BUFFERED_ACCUM_BUFFER)
endif

ifndef SUPPORT_MVM
	export SUPPORT_MVM = false
else
	override BASE_FLAGS += -DSUPPORT_MVM=$(SUPPORT_MVM)
endif

ifndef SUPPORT_SPMM
	export SUPPORT_SPMM = false
else
	override BASE_FLAGS += -DSUPPORT_SPMM=$(SUPPORT_SPMM)
endif

ifndef SUPPORT_DWC
	export SUPPORT_DWC = false
else
	override BASE_FLAGS += -DSUPPORT_DWC=$(SUPPORT_DWC)
endif

ifdef CLOCK_PERIOD
	override BASE_FLAGS += -DCLOCK_PERIOD=$(CLOCK_PERIOD)
endif

ifeq ($(DEBUG), 1)
	override BASE_FLAGS += -DDEBUG -g -O0 -ggdb
else
	override BASE_FLAGS += -O3
endif

ifdef ZERO_INIT
	override BASE_FLAGS += -DZERO_INIT
endif

# We need to work with multiple C++ standards, as the SystemC lib is only
# compatible with C++11 and the Universal Numbers Library requires C++17
C17FLAGS += $(BASE_FLAGS) -std=c++17 -Wno-deprecated-declarations
LDFLAGS += -lsystemc -lstdc++fs -labsl_hash -labsl_log_internal_check_op -labsl_log_internal_message -labsl_log_internal_nullguard -lprotobuf -lpthread -Wl,-rpath=$(CONDA_PREFIX)/lib
LDLIBS += -L$(CATAPULT_ROOT)/shared/lib/ -L$(CONDA_PREFIX)/lib
LDFLAGS_NO_SYSC += -lstdc++fs -labsl_hash -labsl_log_internal_check_op -labsl_log_internal_message -labsl_log_internal_nullguard -lprotobuf -lpthread -Wl,-rpath=$(CONDA_PREFIX)/lib
LDLIBS_NO_SYSC += -L$(CONDA_PREFIX)/lib

###########################################################
# Build Directories
###########################################################
CIM_BUILD_SIGNATURE = ci$(CIM_CH_IN)_co$(CIM_CH_OUT)_bs$(CIM_B_SETS)_base$(CIM_BASE_A_WIDTH)x$(CIM_BASE_B_WIDTH)x$(CIM_BASE_C_WIDTH)_w$(CIM_WRITE_CH_IN)_lat$(CIM_MAC_LATENCY)_m$(CIM_MODE)_tie$(CIM_TILE_INPUT_AXIS_ELEMENTS)_toe$(CIM_TILE_OUTPUT_AXIS_ELEMENTS)_iat$(CIM_INPUT_AXIS_TILES)_oat$(CIM_OUTPUT_AXIS_TILES)_apt$(CIM_A_PORT_TILES)_bpt$(CIM_B_PORT_TILES)_cpt$(CIM_C_PORT_TILES)_cbl$(CIM_C_BEAT_LAYOUT)_rslots$(CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE)_lac$(CIM_LOCAL_ACCUM_CONTEXTS)
# Overridden external port widths change the generated params, so they must not
# share a build directory with the derived-width default
PORT_BUILD_SIGNATURE = $(if $(IC_PORT_WIDTH)$(OC_PORT_WIDTH),_icp$(if $(IC_PORT_WIDTH),$(IC_PORT_WIDTH),auto)_ocp$(if $(OC_PORT_WIDTH),$(OC_PORT_WIDTH),auto))
# Keep the backend explicit so new builds cannot reuse legacy unsuffixed caches
BACKEND_BUILD_SIGNATURE = $(if $(filter 1,$(MATRIX_BACKEND)),_cim_$(CIM_BUILD_SIGNATURE),_sa)
BUILD_DIR ?= build/$(DATATYPE)_$(IC_DIMENSION)x$(OC_DIMENSION)_$(INPUT_BUFFER_SIZE)x$(WEIGHT_BUFFER_SIZE)x$(ACCUM_BUFFER_SIZE)_$(DOUBLE_BUFFERED_ACCUM_BUFFER)_$(SUPPORT_MVM)_$(SUPPORT_SPMM)$(BACKEND_BUILD_SIGNATURE)$(PORT_BUILD_SIGNATURE)
CC_BUILD_DIR = $(BUILD_DIR)/cc

# Report the effective build directory so external tools stay consistent with
# this Makefile instead of duplicating the naming scheme
.PHONY: print-build-dir
print-build-dir:
	@echo $(BUILD_DIR)

# Report the backend-specific SCVerify makefile and arguments to external runners
.PHONY: print-scverify-rtl-config
print-scverify-rtl-config:
	@echo '$(SCVERIFY_RTL_MK)|$(SCVERIFY_RTL_ARGS)'

ALL_BUILD_DIRS = $(CC_BUILD_DIR) $(TOOLCHAIN_BUILD_DIRS)
# Create build dirs automatically
$(info $(shell mkdir -p $(ALL_BUILD_DIRS)))

###########################################################
# spdlog
###########################################################
SPDLOG_OBJ_FILES = $(patsubst lib/spdlog/src/%.cpp,$(CC_BUILD_DIR)/spdlog_%.o,$(wildcard lib/spdlog/src/*.cpp))

$(CC_BUILD_DIR)/spdlog_%.o: lib/spdlog/src/%.cpp
	$(CC) $(C17FLAGS) -c -o $@ $<

$(CC_BUILD_DIR)/spdlog.o: $(SPDLOG_OBJ_FILES)
	ld -r -o $@ $^

spdlog: $(CC_BUILD_DIR)/spdlog.o

###########################################################
# Catapult Synthesis
###########################################################
export CATAPULT_BUILD_DIR ?= $(BUILD_DIR)/Catapult/$(TECHNOLOGY)/clock_$(CLOCK_PERIOD)

# Main target to run HLS and build RTL (Verilog)
rtl: Accelerator

# Generating RTL requires test/compiler/proto/{param.pb.cc, tiling.pb.cc} to
# exist. But we don't want to add it as a dependency, as it would trigger a
# rebuild of the rtl target every time the proto files change. Instead we create
# a conditional dependency on the proto files, which will only create the proto
# file if it doesn't exist.
PROTOS_DEPENDENCY =
ifeq (,$(wildcard test/compiler/proto/param.pb.cc))
PROTOS_DEPENDENCY += test/compiler/proto/param.pb.cc
endif
ifeq (,$(wildcard test/compiler/proto/tiling.pb.cc))
PROTOS_DEPENDENCY += test/compiler/proto/tiling.pb.cc
endif

RTL_DEPENDENCIES =
ifeq ($(SUPPORT_MVM), true)
RTL_DEPENDENCIES += $(CATAPULT_BUILD_DIR)/MatrixVectorUnit/MatrixVectorUnit.v1/concat_rtl.v
endif
ifeq ($(SUPPORT_SPMM), true)
RTL_DEPENDENCIES += $(CATAPULT_BUILD_DIR)/SpMMUnit/SpMMUnit.v1/concat_rtl.v
endif
ifeq ($(SUPPORT_DWC), true)
RTL_DEPENDENCIES += $(CATAPULT_BUILD_DIR)/DwCUnit/DwCUnit.v1/concat_rtl.v
endif

VU_RTL_DEPENDENCIES =
ifeq ($(SUPPORT_SPMM), true)
VU_RTL_DEPENDENCIES += $(CATAPULT_BUILD_DIR)/OutlierFilter/OutlierFilter.v1/concat_rtl.v
endif

ifeq ($(MATRIX_BACKEND),1)
CIM_ARRAY_RTL := $(CATAPULT_BUILD_DIR)/CIMArray/CIMArray.v1/concat_rtl.sv
MATRIX_BACKEND_RTL := $(CATAPULT_BUILD_DIR)/CIMProcessor/CIMProcessor.v1/concat_rtl.sv
# The CIM netlist is SystemVerilog, so Catapult emits no Verilog concat flow, and
# its SystemVerilog concat flow never builds the ccs_wrapper the harness links
# against; the per-file RTL flow does, and needs the CIM include path
SCVERIFY_RTL_MK := Verify_rtl_v_vcs.mk
SCVERIFY_RTL_ARGS := VLOG_INCDIRS=$(PROJ_ROOT)/src/CIM
MATRIX_UNIT_RTL := $(CATAPULT_BUILD_DIR)/MatrixUnit/MatrixUnit.v1/concat_rtl.sv
ACCELERATOR_RTL := $(CATAPULT_BUILD_DIR)/Accelerator/Accelerator.v1/concat_rtl.sv
else
MATRIX_BACKEND_RTL := $(CATAPULT_BUILD_DIR)/MatrixProcessor/MatrixProcessor.v1/concat_rtl.v
SCVERIFY_RTL_MK := Verify_concat_sim_rtl_v_vcs.mk
SCVERIFY_RTL_ARGS :=
MATRIX_UNIT_RTL := $(CATAPULT_BUILD_DIR)/MatrixUnit/MatrixUnit.v1/concat_rtl.v
ACCELERATOR_RTL := $(CATAPULT_BUILD_DIR)/Accelerator/Accelerator.v1/concat_rtl.v
endif

# For debugging it might be beneficial to only build sub-components in RTL and
# have them integrate into the SystemC code
InputController: $(CATAPULT_BUILD_DIR)/InputController/InputController.v1/concat_rtl.v
WeightController: $(CATAPULT_BUILD_DIR)/WeightController/WeightController.v1/concat_rtl.v
SystolicArray: $(CATAPULT_BUILD_DIR)/SystolicArray/SystolicArray.v1/concat_rtl.v
MatrixProcessor: $(CATAPULT_BUILD_DIR)/MatrixProcessor/MatrixProcessor.v1/concat_rtl.v
MatrixUnit: $(MATRIX_UNIT_RTL)
CIMArray:
	env -u CATAPULT_BUILD_DIR $(MAKE) cim-array-rtl MATRIX_BACKEND=1 CIM_C_BEAT_LAYOUT=1
CIMProcessor:
	env -u CATAPULT_BUILD_DIR $(MAKE) cim-processor-rtl MATRIX_BACKEND=1 CIM_C_BEAT_LAYOUT=1
cim-array-rtl: $(CIM_ARRAY_RTL)
cim-processor-rtl: $(MATRIX_BACKEND_RTL)
ProcessingElement: $(CATAPULT_BUILD_DIR)/ProcessingElement/ProcessingElement.v1/concat_rtl.v
CIMElement: $(CATAPULT_BUILD_DIR)/CIMElement/CIMElement.v1/concat_rtl.v
CIMUnit: $(CATAPULT_BUILD_DIR)/CIMUnit/CIMUnit.v1/concat_rtl.v
VectorFetchUnit: $(CATAPULT_BUILD_DIR)/VectorFetchUnit/VectorFetchUnit.v1/concat_rtl.v
VectorUnit: $(CATAPULT_BUILD_DIR)/VectorUnit/VectorUnit.v1/concat_rtl.v
OutputController: $(CATAPULT_BUILD_DIR)/OutputController/OutputController.v1/concat_rtl.v
VectorPipeline: $(CATAPULT_BUILD_DIR)/VectorPipeline/VectorPipeline.v1/concat_rtl.v
MatrixVectorUnit: $(CATAPULT_BUILD_DIR)/MatrixVectorUnit/MatrixVectorUnit.v1/concat_rtl.v
SpMMUnit: $(CATAPULT_BUILD_DIR)/SpMMUnit/SpMMUnit.v1/concat_rtl.v
MulAddTree: $(CATAPULT_BUILD_DIR)/MulAddTree/MulAddTree.v1/concat_rtl.v
DwCUnit: $(CATAPULT_BUILD_DIR)/DwCUnit/DwCUnit.v1/concat_rtl.v
Accelerator: $(ACCELERATOR_RTL)

$(CATAPULT_BUILD_DIR)/InputController/InputController.v1/concat_rtl.v: src/InputController.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=InputController catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/InputController.log

$(CATAPULT_BUILD_DIR)/WeightController/WeightController.v1/concat_rtl.v: src/WeightController.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=WeightController catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/WeightController.log

$(CATAPULT_BUILD_DIR)/ProcessingElement/ProcessingElement.v1/concat_rtl.v: src/ProcessingElement.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=ProcessingElement catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/ProcessingElement.log

$(CATAPULT_BUILD_DIR)/CIMElement/CIMElement.v1/concat_rtl.v: src/CIMElement.h src/CIM/cim_typedefs.svh src/CIM/cim_macro_wrapper.sv src/CIM/cim_macro_model.sv src/CIM/cim_macro_1.sv src/CIM/cim_element.sv
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=CIMElement catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/CIMElement.log

$(CATAPULT_BUILD_DIR)/CIMUnit/CIMUnit.v1/concat_rtl.v: src/CIMUnit.h src/CIMElement.h src/CIM/cim_typedefs.svh src/CIM/cim_macro_wrapper.sv src/CIM/cim_macro_model.sv src/CIM/cim_macro_1.sv src/CIM/cim_element.sv
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=CIMUnit catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/CIMUnit.log

$(CATAPULT_BUILD_DIR)/SystolicArrayRow/SystolicArrayRow.v1/concat_rtl.v: src/SystolicArray.h $(CATAPULT_BUILD_DIR)/ProcessingElement/ProcessingElement.v1/concat_rtl.v $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=SystolicArrayRow catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/SystolicArrayRow.log

$(CATAPULT_BUILD_DIR)/SystolicArray/SystolicArray.v1/concat_rtl.v: src/SystolicArray.h $(CATAPULT_BUILD_DIR)/SystolicArrayRow/SystolicArrayRow.v1/concat_rtl.v $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=SystolicArray catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/SystolicArray.log

$(CATAPULT_BUILD_DIR)/MatrixProcessor/MatrixProcessor.v1/concat_rtl.v: src/MatrixProcessor.h src/SystolicArray.h src/Skewer.h $(CATAPULT_BUILD_DIR)/SystolicArray/SystolicArray.v1/concat_rtl.v $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=MatrixProcessor catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/MatrixProcessor.log

$(CATAPULT_BUILD_DIR)/CIMArray/CIMArray.v1/concat_rtl.sv: src/CIMArray.h src/CIMTile.h src/CIMElement.h src/CIM/cim_macro_wrapper.sv src/CIM/cim_macro_model.sv src/CIM/cim_macro_1.sv src/CIM/cim_element.sv scripts/blocks/CIMArray.tcl scripts/architecture.tcl scripts/utils/setup_project.tcl $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=CIMArray catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/CIMArray.log

$(CATAPULT_BUILD_DIR)/CIMProcessor/CIMProcessor.v1/concat_rtl.sv: \
	src/CIMProcessor.h \
	src/ArchitectureParams.h \
	src/Params.h \
	src/TypeToBits.h \
	src/Utils.h \
	scripts/blocks/CIMProcessor.tcl \
	scripts/architecture.tcl \
	scripts/utils/setup_project.tcl \
	$(CIM_ARRAY_RTL) \
	$(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=CIMProcessor catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/CIMProcessor.log

$(CATAPULT_BUILD_DIR)/MatrixParamsDeserializer/MatrixParamsDeserializer.v1/concat_rtl.v: src/ParamsDeserializer.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=MatrixParamsDeserializer catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/MatrixParamsDeserializer.log

$(CATAPULT_BUILD_DIR)/VectorFetchUnit/VectorFetchUnit.v1/concat_rtl.v: src/vector_unit/VectorFetch.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=VectorFetchUnit catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/VectorFetchUnit.log

$(CATAPULT_BUILD_DIR)/VectorParamsDeserializer/VectorParamsDeserializer.v1/concat_rtl.v: src/ParamsDeserializer.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=VectorParamsDeserializer catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/VectorParamsDeserializer.log

$(CATAPULT_BUILD_DIR)/OutlierFilter/OutlierFilter.v1/concat_rtl.v: src/vector_unit/OutlierFilter.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=OutlierFilter catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/OutlierFilter.log

$(CATAPULT_BUILD_DIR)/VectorPipeline/VectorPipeline.v1/concat_rtl.v: $(VU_RTL_DEPENDENCIES) src/vector_unit/VectorPipeline.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=VectorPipeline catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/VectorPipeline.log

$(CATAPULT_BUILD_DIR)/VectorReducer/VectorReducer.v1/concat_rtl.v: src/vector_unit/Reducer.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=VectorReducer catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/VectorReducer.log

$(CATAPULT_BUILD_DIR)/VectorAccumulator/VectorAccumulator.v1/concat_rtl.v: src/vector_unit/Accumulator.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=VectorAccumulator catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/VectorAccumulator.log

$(CATAPULT_BUILD_DIR)/OutputController/OutputController.v1/concat_rtl.v: src/vector_unit/OutputController.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=OutputController catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/OutputController.log

$(CATAPULT_BUILD_DIR)/VectorUnit/VectorUnit.v1/concat_rtl.v: \
    $(CATAPULT_BUILD_DIR)/VectorFetchUnit/VectorFetchUnit.v1/concat_rtl.v \
    $(CATAPULT_BUILD_DIR)/VectorPipeline/VectorPipeline.v1/concat_rtl.v \
	$(CATAPULT_BUILD_DIR)/VectorReducer/VectorReducer.v1/concat_rtl.v \
	$(CATAPULT_BUILD_DIR)/VectorAccumulator/VectorAccumulator.v1/concat_rtl.v \
    $(CATAPULT_BUILD_DIR)/OutputController/OutputController.v1/concat_rtl.v \
    $(CATAPULT_BUILD_DIR)/VectorParamsDeserializer/VectorParamsDeserializer.v1/concat_rtl.v \
	src/vector_unit/main.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=VectorUnit catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/VectorUnit.log

$(CATAPULT_BUILD_DIR)/VectorMacUnit/VectorMacUnit.v1/concat_rtl.v: src/matrix_vector_unit/VectorMacUnit.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=VectorMacUnit catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/VectorMacUnit.log

$(CATAPULT_BUILD_DIR)/MatrixVectorUnit/MatrixVectorUnit.v1/concat_rtl.v: \
	$(CATAPULT_BUILD_DIR)/VectorMacUnit/VectorMacUnit.v1/concat_rtl.v \
	src/matrix_vector_unit/main.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=MatrixVectorUnit catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/MatrixVectorUnit.log

$(CATAPULT_BUILD_DIR)/SpMMUnit/SpMMUnit.v1/concat_rtl.v: src/SpMMUnit.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=SpMMUnit catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/SpMMUnit.log

$(CATAPULT_BUILD_DIR)/MulAddTree/MulAddTree.v1/concat_rtl.v: src/MulAddTree.h $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=MulAddTree catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/MulAddTree.log
$(CATAPULT_BUILD_DIR)/DwCUnit/DwCUnit.v1/concat_rtl.v: src/DwCUnit.h $(CATAPULT_BUILD_DIR)/MulAddTree/MulAddTree.v1/concat_rtl.v $(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=DwCUnit catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/DwCUnit.log

$(MATRIX_UNIT_RTL): \
	src/MatrixUnit.h \
	src/DoubleBuffer.h \
	scripts/blocks/MatrixUnit.tcl \
	scripts/architecture.tcl \
	scripts/utils/setup_project.tcl \
	$(CATAPULT_BUILD_DIR)/InputController/InputController.v1/concat_rtl.v \
	$(CATAPULT_BUILD_DIR)/WeightController/WeightController.v1/concat_rtl.v \
	$(CATAPULT_BUILD_DIR)/MatrixParamsDeserializer/MatrixParamsDeserializer.v1/concat_rtl.v \
	$(MATRIX_BACKEND_RTL) \
	$(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=MatrixUnit catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/MatrixUnit.log

$(ACCELERATOR_RTL): \
	src/Accelerator.h \
	src/DoubleBuffer.h \
	src/MatrixUnit.h \
	scripts/blocks/Accelerator.tcl \
	scripts/architecture.tcl \
	scripts/utils/setup_project.tcl \
	$(CATAPULT_BUILD_DIR)/InputController/InputController.v1/concat_rtl.v \
	$(CATAPULT_BUILD_DIR)/WeightController/WeightController.v1/concat_rtl.v \
	$(MATRIX_BACKEND_RTL) \
	$(CATAPULT_BUILD_DIR)/VectorUnit/VectorUnit.v1/concat_rtl.v \
	$(CATAPULT_BUILD_DIR)/MatrixParamsDeserializer/MatrixParamsDeserializer.v1/concat_rtl.v \
	$(RTL_DEPENDENCIES) \
	$(PROTOS_DEPENDENCY)
	mkdir -p $(CATAPULT_BUILD_DIR)
	BLOCK=Accelerator catapult -shell -file scripts/main.tcl -logfile $(CATAPULT_BUILD_DIR)/Accelerator.log

.PHONY: rtl Accelerator InputController WeightController MatrixProcessor MatrixUnit CIMArray CIMProcessor cim-array-rtl cim-processor-rtl ProcessingElement CIMElement CIMUnit VectorUnit VectorParamsDeserializer VectorFetchUnit VectorPipeline VectorReducer VectorAccumulator OutputController MatrixVectorUnit MulAddTree DwCUnit

# Run RTL simulation
.PHONY: rtl-sim
rtl-sim: rtl network-proto
	cd $(CATAPULT_BUILD_DIR)/Accelerator/Accelerator.v1 && LD_PRELOAD=$(CONDA_PREFIX)/lib/libstdc++.so.6 make -f ./scverify/$(SCVERIFY_RTL_MK) SIMTOOL=vcs $(SCVERIFY_RTL_ARGS) sim

.PHONY: rtl-sim-debug
rtl-sim-debug: rtl network-proto
	cd $(CATAPULT_BUILD_DIR)/Accelerator/Accelerator.v1 && LD_PRELOAD=$(CONDA_PREFIX)/lib/libstdc++.so.6 SIM_DUMP_FSDB=1 make -f ./scverify/$(SCVERIFY_RTL_MK) SIMTOOL=vcs $(SCVERIFY_RTL_ARGS) sim

###########################################################
# Standard Event-based SystemC Simulations
###########################################################

# Main target for accelerator simulations
.PHONY: sim
sim: $(CC_BUILD_DIR)/TestRunner network-proto
	./$(CC_BUILD_DIR)/TestRunner

.PHONY: fast-sim
fast-sim: $(CC_BUILD_DIR)/TestRunner-fast network-proto
	./$(CC_BUILD_DIR)/TestRunner-fast

.PHONY: fast-sim-check
fast-sim-check: $(CC_BUILD_DIR)/TestRunner-checker network-proto
	./$(CC_BUILD_DIR)/TestRunner-checker

.PHONY: sim-debug
sim-debug: $(CC_BUILD_DIR)/TestRunner network-proto
	gdb ./$(CC_BUILD_DIR)/TestRunner

.PHONY: fast-sim-debug
fast-sim-debug: $(CC_BUILD_DIR)/TestRunner-fast network-proto
	gdb ./$(CC_BUILD_DIR)/TestRunner-fast

.PHONY: TestRunner
TestRunner: check_env_var $(CC_BUILD_DIR)/TestRunner

.PHONY: TestRunner-fast
TestRunner-fast: check_env_var $(CC_BUILD_DIR)/TestRunner-fast

.PHONY: TestRunner-checker
TestRunner-checker: check_env_var $(CC_BUILD_DIR)/TestRunner-checker

.PHONY: AccuracyTester
AccuracyTester: ./$(CC_BUILD_DIR)/AccuracyTester

.PHONY: MobileBertAccuracy
MobileBertAccuracy: $(CC_BUILD_DIR)/AccuracyTester
	./$(CC_BUILD_DIR)/AccuracyTester mobilebert data/bert_sst2_val 64

.PHONY: ResNetAccuracy
ResNetAccuracy: $(CC_BUILD_DIR)/AccuracyTester
	./$(CC_BUILD_DIR)/AccuracyTester resnet18 data/imagenet_val 64

$(CC_BUILD_DIR)/TestRunner: $(CC_BUILD_DIR)/Harness.o $(CC_BUILD_DIR)/TestRunner.o $(CC_BUILD_DIR)/GoldModel.o $(CC_BUILD_DIR)/Simulation.o $(CC_BUILD_DIR)/ArrayMemory.o $(CC_BUILD_DIR)/DataLoader.o $(CC_BUILD_DIR)/Network.o $(CC_BUILD_DIR)/param.pb.o $(CC_BUILD_DIR)/tiling.pb.o $(CC_BUILD_DIR)/MapOperation.o $(CC_BUILD_DIR)/AccessCounter.o $(CC_BUILD_DIR)/Tiling.o  $(SPDLOG_OBJ_FILES)
	$(CC) -o $@ $^ $(LDLIBS) $(LDFLAGS)

$(CC_BUILD_DIR)/TestRunner-fast: $(CC_BUILD_DIR)/Harness-fast.o $(CC_BUILD_DIR)/TestRunner.o $(CC_BUILD_DIR)/GoldModel.o $(CC_BUILD_DIR)/Simulation.o $(CC_BUILD_DIR)/ArrayMemory.o $(CC_BUILD_DIR)/DataLoader.o $(CC_BUILD_DIR)/Network.o $(CC_BUILD_DIR)/param.pb.o $(CC_BUILD_DIR)/tiling.pb.o $(CC_BUILD_DIR)/MapOperation.o $(CC_BUILD_DIR)/AccessCounter.o $(CC_BUILD_DIR)/Tiling.o $(SPDLOG_OBJ_FILES)
	$(CC) -o $@ $^ $(LDLIBS) $(LDFLAGS)

$(CC_BUILD_DIR)/TestRunner-checker: $(CC_BUILD_DIR)/Harness-checker.o $(CC_BUILD_DIR)/TestRunner.o $(CC_BUILD_DIR)/GoldModel-checker.o $(CC_BUILD_DIR)/Simulation.o $(CC_BUILD_DIR)/ArrayMemory.o $(CC_BUILD_DIR)/DataLoader.o $(CC_BUILD_DIR)/Network.o $(CC_BUILD_DIR)/param.pb.o  $(CC_BUILD_DIR)/tiling.pb.o $(CC_BUILD_DIR)/MapOperation.o $(CC_BUILD_DIR)/PEChecker.o $(CC_BUILD_DIR)/AccessCounter.o $(CC_BUILD_DIR)/Tiling.o $(SPDLOG_OBJ_FILES)
	$(CC) -o $@ $^ $(LDLIBS) $(LDFLAGS)

$(CC_BUILD_DIR)/AccuracyTester: $(CC_BUILD_DIR)/AccuracyTester.o $(CC_BUILD_DIR)/GoldModel.o $(CC_BUILD_DIR)/ArrayMemory.o $(CC_BUILD_DIR)/DataLoader.o $(CC_BUILD_DIR)/Network.o $(CC_BUILD_DIR)/param.pb.o $(CC_BUILD_DIR)/tiling.pb.o $(CC_BUILD_DIR)/Tiling.o $(SPDLOG_OBJ_FILES)
	$(CC) -o $@ $^ $(LDLIBS_NO_SYSC) $(LDFLAGS_NO_SYSC) -pthread

$(CC_BUILD_DIR)/Harness.o: test/common/Harness.cc test/common/Harness.h test/common/Utils.h test/toolchain/MapOperation.h $(wildcard src/*.h) $(wildcard src/datatypes/*.h) $(wildcard src/vector_unit/*.h) $(wildcard src/matrix_vector_unit/*.h)
	$(CC) $(C17FLAGS) -c -o $@ $<

$(CC_BUILD_DIR)/Harness-fast.o: test/common/Harness.cc test/common/Harness.h test/common/Utils.h test/toolchain/MapOperation.h $(wildcard src/*.h) $(wildcard src/datatypes/*.h) $(wildcard src/vector_unit/*.h) $(wildcard src/matrix_vector_unit/*.h)
	$(CC) $(C17FLAGS) -DCONNECTIONS_FAST_SIM -c -o $@ $<

$(CC_BUILD_DIR)/Harness-checker.o: test/common/Harness.cc test/common/Harness.h test/common/Utils.h test/toolchain/MapOperation.h $(wildcard src/*.h) $(wildcard src/datatypes/*.h) $(wildcard src/vector_unit/*.h) $(wildcard src/matrix_vector_unit/*.h) test/checker/PEChecker.h
	$(CC) $(C17FLAGS) -DCONNECTIONS_FAST_SIM -DCHECK_PE -c -o $@ $<

$(CC_BUILD_DIR)/GoldModel.o: test/common/GoldModel.cc test/common/GoldModel.h test/common/Utils.h test/common/Tiling.h src/ArchitectureParams.h src/vector_unit/ApproximationUnit.h test/toolchain/ApproximationConstants.h $(wildcard src/datatypes/*.h) $(wildcard test/common/operations/*.h)
	$(CC) $(C17FLAGS) -g -c -o $@ $<

$(CC_BUILD_DIR)/GoldModel-checker.o: test/common/GoldModel.cc test/common/GoldModel.h test/common/Utils.h test/common/Tiling.h src/ArchitectureParams.h $(wildcard src/datatypes/*.h) $(wildcard test/common/operations/*.h) test/checker/PEChecker.h
	$(CC) $(C17FLAGS) -DCHECK_PE -g -c -o $@ $<

$(CC_BUILD_DIR)/Simulation.o: test/common/Simulation.cc test/common/Simulation.h src/ArchitectureParams.h $(wildcard src/datatypes/*.h) test/common/Utils.h test/common/MemoryInterface.h
	$(CC) $(C17FLAGS) -c -o $@ $<

$(CC_BUILD_DIR)/ArrayMemory.o: test/common/ArrayMemory.cc test/common/ArrayMemory.h test/common/MemoryInterface.h
	$(CC) $(C17FLAGS) -c -o $@ $<

$(CC_BUILD_DIR)/DataLoader.o: test/common/DataLoader.cc test/common/DataLoader.h test/common/MemoryInterface.h
	$(CC) $(C17FLAGS) -c -o $@ $<

$(CC_BUILD_DIR)/MapOperation.o: test/toolchain/MapOperation.cc test/common/Tiling.h $(wildcard test/toolchain/*.h)
	$(CC) $(C17FLAGS) -c -o $@ $<

$(CC_BUILD_DIR)/TestRunner.o: test/common/TestRunner.cc
	$(CC) $(C17FLAGS) -c -o $@ $<

$(CC_BUILD_DIR)/PEChecker.o: test/checker/PEChecker.cc test/checker/PEChecker.h $(wildcard src/datatypes/*.h)
	$(CC) $(C17FLAGS) -c -o $@ $<

$(CC_BUILD_DIR)/AccuracyTester.o: test/common/AccuracyTester.cc $(wildcard src/datatypes/*.h) $(wildcard test/toolchain/*.h)
	$(CC) $(C17FLAGS) -c -o $@ $<

$(CC_BUILD_DIR)/AccessCounter.o: test/common/AccessCounter.cc test/common/AccessCounter.h
	$(CC) $(C17FLAGS) -c -o $@ $<

$(CC_BUILD_DIR)/Tiling.o: test/common/Tiling.cc test/common/Tiling.h
	$(CC) $(C17FLAGS) -c -o $@ $<

###########################################################
# Toolchain
###########################################################
toolchain: $(CC_BUILD_DIR)/MapOperation.o $(CC_BUILD_DIR)/Tiling.o $(CC_BUILD_DIR)/Network.o $(CC_BUILD_DIR)/param.pb.o $(CC_BUILD_DIR)/tiling.pb.o

###########################################################
# Networks
###########################################################
$(CC_BUILD_DIR)/Network.o: test/common/Network.cc test/compiler/proto/param.pb.cc
	$(CC) $(C17FLAGS) -c -o $@ $<

test/compiler/proto/param.pb.cc: voyager-compiler/src/voyager_compiler/codegen/param.proto
	protoc -I=voyager-compiler/src/voyager_compiler/codegen --cpp_out=test/compiler/proto $<

test/compiler/proto/tiling_pb2.py: test/compiler/proto/tiling.proto
	protoc --proto_path=test/compiler/proto/ --python_out=test/compiler/proto $<

test/compiler/proto/tiling.pb.cc: test/compiler/proto/tiling.proto
	protoc -I=test/compiler/proto --cpp_out=test/compiler/proto $<

$(CC_BUILD_DIR)/param.pb.o: test/compiler/proto/param.pb.cc
	$(CC) $(C17FLAGS) -c -o $@ $<

$(CC_BUILD_DIR)/tiling.pb.o: test/compiler/proto/tiling.pb.cc
	$(CC) $(C17FLAGS) -c -o $@ $<

.PHONY: network-proto
network-proto: \
    $(CODEGEN_DIR)/networks/$(NETWORK)/$(DATATYPE)/$(CODEGEN_BACKEND_GEOMETRY)/model.txt \
    test/compiler/proto/param.pb.cc \
    test/compiler/proto/tiling_pb2.py \
    test/compiler/proto/tiling.pb.cc \
    $(CODEGEN_DIR)/networks/$(NETWORK)/$(DATATYPE)/$(CODEGEN_BACKEND_GEOMETRY)/$(IC_DIMENSION)x$(OC_DIMENSION)_$(INPUT_BUFFER_SIZE)x$(WEIGHT_BUFFER_SIZE)x$(ACCUM_BUFFER_SIZE)_$(DOUBLE_BUFFERED_ACCUM_BUFFER)/tilings.txtpb

include codegen.mk

###########################################################
# Cleanup Targets
###########################################################

clean-all:
	rm -rf build/*

clean: check_env_var
	rm -rf $(CC_BUILD_DIR)

clean-test:
	rm -rf test_outputs/*

clean-catapult: check_env_var
	rm -rf $(CATAPULT_BUILD_DIR)

clean-rtl-sim: check_env_var
	rm -rf $(CATAPULT_BUILD_DIR)/Accelerator/Accelerator.v1/scverify/concat_sim_rtl_v_vcs

clean-protos:
	rm -rf $(CODEGEN_DIR)/networks/*
	rm -rf test/compiler/proto/*.pb.*

.PHONY: clean clean-test clean-catapult clean-rtl-sim
