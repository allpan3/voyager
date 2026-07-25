# Makefile targets for different codegen models/datatypes
# Each target includes the hardware-unrolling geometry because model codegen
# pads and transforms tensors according to IC_DIMENSION and OC_DIMENSION

E4M3_FLAGS := --activation fp8_e4m3 --weight fp8_e4m3 --bf16
P8_1_FLAGS := --activation posit8_1 --weight posit8_1 --bf16
INT8_FLAGS := --activation int8,qs=per_tensor_symmetric --weight int8,qs=per_tensor_symmetric --bias int24 --bf16 --calibration_steps 3
INT8_32_FLAGS := --activation int8,qs=per_tensor_symmetric --weight int8,qs=per_tensor_symmetric --bias int32 --bf16 --calibration_steps 3
BLOCK_SIZE := $(shell [ $(IC_DIMENSION) -gt $(OC_DIMENSION) ] && echo $(IC_DIMENSION) || echo $(OC_DIMENSION))
MXINT8_FLAGS := --activation int8,qs=microscaling,bs=$(BLOCK_SIZE) --weight int8,qs=microscaling,bs=$(BLOCK_SIZE) --force_scale_power_of_two --bf16
MXNF4_FLAGS := --activation nf4_6,qs=microscaling,bs=$(BLOCK_SIZE),scale=fp8_e5m3 --weight nf4_6,qs=microscaling,bs=$(BLOCK_SIZE),scale=fp8_e5m3 --bf16 --residual fp8_e4m3 --quantize_fc
COMMON_FLAGS := --transform_layout --hardware_unrolling $(IC_DIMENSION),$(OC_DIMENSION) --dump_tensors
EXTRA_COMPILER_FLAGS ?=

CONTEXT ?= 1024
LLM_FLAGS := --context_length $(CONTEXT) --compile_single_layer --quantize_attention_mask

# Set default values if not already defined in the environment
CACHE_SIZE ?= 8388608
NUM_BANKS  ?= 8

ifeq ($(SOC_SIM),1)
COMMON_FLAGS += --cache_size $(CACHE_SIZE) --num_banks $(NUM_BANKS)
endif

ifeq ($(CONV2D_IM2COL),1)
COMMON_FLAGS += --conv2d_im2col
endif

################################################################################
$(CODEGEN_DIR)/networks/resnet18/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py resnet18 $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) > $(dir $@)/codegen.log 2>&1

$(CODEGEN_DIR)/networks/resnet50/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py resnet50 $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) > $(dir $@)/codegen.log 2>&1

$(CODEGEN_DIR)/networks/mobilebert/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py mobilebert $($*_FLAGS) --model_name_or_path models/mobilebert/mobilebert-tiny-sst2-bf16 $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) > $(dir $@)/codegen.log 2>&1

$(CODEGEN_DIR)/networks/mobilebert_encoder/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py mobilebert $($*_FLAGS) --model_name_or_path models/mobilebert/mobilebert-tiny-sst2-bf16 $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) --compile_single_layer > $(dir $@)/codegen.log 2>&1

$(CODEGEN_DIR)/networks/bert/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py bert $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) &> $(dir $@)codegen.log

$(CODEGEN_DIR)/networks/llama_prefill/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py llm_prefill $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) $(LLM_FLAGS) &> $(dir $@)codegen.log

$(CODEGEN_DIR)/networks/llama_decode/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py llm_decode $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) $(LLM_FLAGS) &> $(dir $@)codegen.log

$(CODEGEN_DIR)/networks/llama_decode_kivi/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py llm_kivi $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) $(LLM_FLAGS) &> $(dir $@)codegen.log

$(CODEGEN_DIR)/networks/llama_prefill_mp/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py llm_prefill $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) $(LLM_FLAGS) --enable_mixed_precision &> $(dir $@)codegen.log

$(CODEGEN_DIR)/networks/llama_prefill_spmm/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py llm_prefill $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) $(LLM_FLAGS) --enable_mixed_precision --outlier_pct 0.01 &> $(dir $@)codegen.log

$(CODEGEN_DIR)/networks/llama_decode_mp/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py llm_decode $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) $(LLM_FLAGS) --enable_mixed_precision &> $(dir $@)codegen.log

$(CODEGEN_DIR)/networks/vit/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py vit $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) &> $(dir $@)codegen.log

$(CODEGEN_DIR)/networks/segformer/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python -u voyager-compiler/test/test_codegen.py segformer $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) &> $(dir $@)codegen.log

$(CODEGEN_DIR)/networks/mobilenet_v2/%/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py mobilenet_v2 $($*_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) &> $(dir $@)codegen.log

################################################################################
# Gesture
################################################################################
$(CODEGEN_DIR)/networks/gesture/CFLOAT/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py gesture --model_name_or_path models/gesture/model.pth --model_output_dir $(dir $@) > $(dir $@)/codegen.log 2>&1

################################################################################
# Layer Tests
################################################################################
test/compiler/networks/layertest/CFLOAT/$(CODEGEN_BACKEND_GEOMETRY)/model.txt: voyager-compiler/test/test_codegen.py
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py layertest --model_output_dir $(dir $@) > $(dir $@)/codegen.log 2>&1

################################################################################
# Tilings
################################################################################
$(CODEGEN_DIR)/networks/$(NETWORK)/$(DATATYPE)/$(CODEGEN_BACKEND_GEOMETRY)/$(IC_DIMENSION)x$(OC_DIMENSION)_$(INPUT_BUFFER_SIZE)x$(WEIGHT_BUFFER_SIZE)x$(ACCUM_BUFFER_SIZE)_$(DOUBLE_BUFFERED_ACCUM_BUFFER)/tilings.txtpb: $(CODEGEN_DIR)/networks/$(NETWORK)/$(DATATYPE)/$(CODEGEN_BACKEND_GEOMETRY)/model.txt test/compiler/run_tiler.py test/compiler/proto/tiling_pb2.py
	mkdir -p $(dir $@)
	python test/compiler/run_tiler.py --codegen_dir $(dir $<) --IC_dimension $(IC_DIMENSION) --OC_dimension $(OC_DIMENSION) --input_buffer_size $(INPUT_BUFFER_SIZE) --weight_buffer_size $(WEIGHT_BUFFER_SIZE) --accum_buffer_size $(ACCUM_BUFFER_SIZE) $(if $(filter true,$(DOUBLE_BUFFERED_ACCUM_BUFFER)), --double_buffered_accum_buffer) > $(dir $@)/tiler.log 2>&1
