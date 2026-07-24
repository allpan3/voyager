#!/bin/bash -l
# CIM vs systolic sweep over MobileBERT matrix-unit layers.
#
# Usage: cmp_sweep.sh <results_dir> <port_width_bits|auto> [config ...]
#
#   port_width_bits  Pins IC_PORT_WIDTH and OC_PORT_WIDTH, so every design point
#                    sees the same external (L2) interface: one word per cycle
#                    per port, i.e. port_width/8 bytes/cycle each for the input,
#                    weight, bias and output ports.
#   auto             Leaves them unset, so ArchitectureParams.h derives them from
#                    the array dimensions and external bandwidth scales with the
#                    array. Reproduces the original derived-width runs.
#   config ...       Optional subset of the table below; default is all of them.
#
# Log files are named <config>[_pw<bits>]__<layer>.log, which cmp_parse.py reads.
set +u
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1   # repo root
source .envrc >/dev/null 2>&1          # env.sh hard-exits under set -u
export PYTHONPATH="$PWD/voyager-compiler/src${PYTHONPATH:+:$PYTHONPATH}"
ulimit -s unlimited                    # 64x64 Harness is a ~33 MB stack object

RES=$1
PW=$2
shift 2
if [ -z "$RES" ] || [ -z "$PW" ]; then
  sed -n '2,17p' "${BASH_SOURCE[0]}"
  exit 2
fi
mkdir -p "$RES"

# Codegen corpus for configurations with an unroll dimension of 64 or more
CODEGEN_DIR_64=${CODEGEN_DIR_64:-cmp_results/compiler64}

export DATATYPE=INT8 INPUT_BUFFER_SIZE=1024 WEIGHT_BUFFER_SIZE=1024 ACCUM_BUFFER_SIZE=1024
export CLOCK_PERIOD=5 NETWORK=mobilebert_encoder SIMS=gold,accelerator

if [ "$PW" = "auto" ]; then
  unset IC_PORT_WIDTH OC_PORT_WIDTH
  SUFFIX=""
else
  export IC_PORT_WIDTH=$PW OC_PORT_WIDTH=$PW
  SUFFIX="_pw${PW}"
fi

LAYERS=(
  mobilebert_encoder_layer_0_ffn_0_output_dense_fused        # M=128 K=512 N=128
  mobilebert_encoder_layer_0_output_bottleneck_dense_fused   # M=128 K=128 N=512
  mobilebert_encoder_layer_0_attention_output_dense_fused    # M=512 K=128 N=128
  matmul_6_fused                                             # M=128 K=128 N=32, act x act
  matmul_2_fused                                             # M=128 K=32  N=128, act x act
)

# CIM geometry: TILE_N = TOE * CH_OUT / (8 / BASE_B), so N = TILE_N * OAT.
# Keeping B_PORT_TILES = C_PORT_TILES = OAT delivers one whole B row per beat.
# name           backend IC OC BASE_A BASE_B BASE_C TOE OAT
ALL=(
  "sa_32x32          0 32 32  0 0  0  0  0"
  "sa_32x64          0 32 64  0 0  0  0  0"
  "sa_64x32          0 64 32  0 0  0  0  0"
  "sa_64x64          0 64 64  0 0  0  0  0"
  "cimb8_32x32       1 32 32  8 8 24  4  1"
  "cimb8_32x64       1 32 64  8 8 24  4  2"
  "cimb8_64x32       1 64 32  8 8 24  4  1"
  "cimb8_64x64       1 64 64  8 8 24  4  2"
  "cim_32x32         1 32 32  4 4 20  2  4"
  "cim_32x64         1 32 64  4 4 20  4  4"
  "cim_64x32         1 64 32  4 4 20  2  4"
  "cim_64x64         1 64 64  4 4 20  4  4"
  "cimflat_64x64     1 64 64  4 4 20  1 16"
)

WANT=("$@")
want() {
  [ ${#WANT[@]} -eq 0 ] && return 0
  for w in "${WANT[@]}"; do [ "$w" = "$1" ] && return 0; done
  return 1
}

for cfg in "${ALL[@]}"; do
  read -r name backend ic oc ba bb bc toe oat <<< "$cfg"
  want "$name" || continue
  echo "=== [$(date +%T)] $name (${ic}x${oc}) port ${PW} ==="
  export MATRIX_BACKEND=$backend IC_DIMENSION=$ic OC_DIMENSION=$oc
  # The compiler disables reshape fusion once an unroll dim reaches 64, so >=64
  # configurations need their own codegen corpus
  if [ "$ic" -ge 64 ] || [ "$oc" -ge 64 ]; then
    export CODEGEN_DIR=$CODEGEN_DIR_64
  else
    export CODEGEN_DIR=test/compiler
  fi
  if [ "$backend" = "1" ]; then
    export CIM_CH_IN=$ic CIM_CH_OUT=8 CIM_B_SETS=2 \
           CIM_BASE_A_WIDTH=$ba CIM_BASE_B_WIDTH=$bb CIM_BASE_C_WIDTH=$bc \
           CIM_WRITE_CH_IN=1 CIM_MAC_LATENCY=1 CIM_MODE=0 \
           CIM_TILE_INPUT_AXIS_ELEMENTS=1 CIM_TILE_OUTPUT_AXIS_ELEMENTS=$toe \
           CIM_INPUT_AXIS_TILES=1 CIM_OUTPUT_AXIS_TILES=$oat \
           CIM_A_PORT_TILES=1 CIM_B_PORT_TILES=$oat CIM_C_PORT_TILES=$oat \
           CIM_C_BEAT_LAYOUT=1
  fi

  tag="${name}${SUFFIX}"
  make network-proto > "$RES/${tag}_proto.log" 2>&1 || { echo "  PROTO FAILED"; continue; }
  make -j32 TestRunner > "$RES/${tag}_build.log" 2>&1 || {
    echo "  BUILD FAILED"; tail -20 "$RES/${tag}_build.log"; continue; }
  for layer in "${LAYERS[@]}"; do
    ( TESTS=$layer timeout 7200 make sim > "$RES/${tag}__${layer}.log" 2>&1 ) &
  done
  wait
  echo "  done $(date +%T)"
done
echo "ALL DONE $(date +%T)"
