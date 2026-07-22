set shell := ["bash", "-lc"]

export DATATYPE := env_var_or_default("DATATYPE", "INT8")
export IC_DIMENSION := env_var_or_default("IC_DIMENSION", "16")
export OC_DIMENSION := env_var_or_default("OC_DIMENSION", "16")
export INPUT_BUFFER_SIZE := env_var_or_default("INPUT_BUFFER_SIZE", "1024")
export WEIGHT_BUFFER_SIZE := env_var_or_default("WEIGHT_BUFFER_SIZE", "1024")
export ACCUM_BUFFER_SIZE := env_var_or_default("ACCUM_BUFFER_SIZE", "1024")
export CLOCK_PERIOD := env_var_or_default("CLOCK_PERIOD", "5")

SIM := env_var_or_default("SIM", "fast-systemc")
PROCESSES := env_var_or_default("PROCESSES", "16")

# List available recipes
default:
    @just --list

# Run one or more comma-separated MobileBERT encoder tests
test-mobilebert-encoder tests:
    source .envrc && \
        export PYTHONPATH="$PWD/voyager-compiler/src${PYTHONPATH:+:$PYTHONPATH}" && \
        NETWORK="mobilebert_encoder" make network-proto && \
        python run_regression.py \
            --models "mobilebert_encoder" \
            --tests "{{ tests }}" \
            --sims "{{ SIM }}" \
            --num_processes "{{ PROCESSES }}"

# Run every unique MobileBERT encoder layer using CI skip rules
regression-mobilebert-encoder:
    source .envrc && \
        export PYTHONPATH="$PWD/voyager-compiler/src${PYTHONPATH:+:$PYTHONPATH}" && \
        python run_regression.py \
            --models "mobilebert_encoder" \
            --sims "{{ SIM }}" \
            --num_processes "{{ PROCESSES }}" \
            --uniquify_layers \
            --skip_layers

# Run every unique MobileBERT encoder layer with the validated 16x16 CIM geometry
regression-cim-mobilebert-encoder:
    source .envrc && \
        export PYTHONPATH="$PWD/voyager-compiler/src${PYTHONPATH:+:$PYTHONPATH}" && \
        DATATYPE=INT8 \
        IC_DIMENSION=16 \
        OC_DIMENSION=16 \
        INPUT_BUFFER_SIZE=1024 \
        WEIGHT_BUFFER_SIZE=1024 \
        ACCUM_BUFFER_SIZE=1024 \
        MATRIX_BACKEND=1 \
        CIM_CH_IN=16 \
        CIM_CH_OUT=8 \
        CIM_B_SETS=2 \
        CIM_BASE_A_WIDTH=4 \
        CIM_BASE_B_WIDTH=4 \
        CIM_BASE_C_WIDTH=20 \
        CIM_WRITE_CH_IN=1 \
        CIM_MAC_LATENCY=1 \
        CIM_MODE=0 \
        CIM_TILE_INPUT_AXIS_ELEMENTS=1 \
        CIM_TILE_OUTPUT_AXIS_ELEMENTS=1 \
        CIM_INPUT_AXIS_TILES=1 \
        CIM_OUTPUT_AXIS_TILES=4 \
        CIM_A_PORT_TILES=1 \
        CIM_B_PORT_TILES=4 \
        CIM_C_PORT_TILES=4 \
        CIM_C_BEAT_LAYOUT=1 \
        python run_regression.py \
            --models "mobilebert_encoder" \
            --sims "systemc" \
            --num_processes "{{ PROCESSES }}" \
            --uniquify_layers \
            --skip_layers
