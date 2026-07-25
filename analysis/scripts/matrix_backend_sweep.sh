#!/bin/bash -l
# CIM vs systolic data-collection sweep over MobileBERT matrix-unit layers.
#
# This is a data-collection tool, not a verification tool: it exists to produce
# cycle-accurate RTL numbers, so RTL cosim runs by default. Each config is
# synthesized with Catapult and cosimulated with VCS; the fast SystemC pass runs
# first only as a pre-synthesis gate (skip a functionally broken config before
# paying ~24 min of synthesis) and as a reference row in the output.
#
# Usage: matrix_backend_sweep.sh <results_dir> [config ...]
#
#   config ...       Optional subset of the generated configs (by name); default
#                    is all of them. Port width is a swept axis, not an argument
#                    -- see PORT_WIDTHS below.
#
# Env toggles:  RTL=0      skip synthesis/cosim and run the SystemC gate only
#                          (default is RTL=1; only use RTL=0 for a quick smoke check)
#               DRY_RUN=1  print the expanded design space and exit (no build)
#
# ============================ DESIGN SPACE ===================================
# A CIM design point is fully described by (array K x N, macro cell width, macro
# CH_IN, macro CH_OUT); the tiling to reach K x N is derived below, so only
# these knobs need touching.
#
#   SWEEP_SET   featured | full   (env-overridable, default featured)
#     featured  one native CIM point per array size, plus the systolic baseline
#               -- the headline comparison, cheap enough for the RTL flow
#     full      the complete cross of the axis arrays below
#
#   GEOMETRIES     target array dimensions, "K x N" (= IC x OC of the layer map)
#   CELL_BITS      CIM macro cell widths; 8 matches the INT8 datapath (native),
#                  4 is the vanilla sub-word macro (two cells per INT8 weight)
#   CH_OUT_VALUES  macro output channels; N is reached by tiling these
#   CH_IN_DEFAULT  macro input channels unless an override applies; K is reached
#                  by tiling these (K / CH_IN input-axis tiles). "geom" => K
#   CH_IN_EXTRA    extra CH_IN constructions for specific geometries,
#                  "KxN:CH_IN" (adds points; does not replace the default)
#   INCLUDE_SYSTOLIC  1 to also emit the systolic baseline for each geometry
#
#   BASELINE_GEOMETRY  the systolic array every design point is judged against.
#                  Its dimensions set the "baseline" port width below, so the
#                  constant-bandwidth point follows the baseline automatically
#                  instead of being a magic number.
#
# Every design point is run at two external (L2) port widths. The interface model
# is one word per cycle per port (width/8 B/cyc each for input, weight, bias and
# output), and the two points answer two different questions:
#
#   baseline   the baseline array's width -- SA 32x32 INT8 gives 32 B/cyc. Holds
#              the external interface fixed while the array grows underneath it,
#              which is the honest comparison when a bigger CIM array is being
#              offered as a replacement for the baseline.
#   matched    this design's own width -- a 64x64 CIM array gets 64 B/cyc. This
#              is what the array can actually absorb, and it is exactly what
#              ArchitectureParams.h derives when the widths are left unset.
#
# They coincide for a design the same size as the baseline, and that duplicate is
# dropped rather than built twice -- "matched" is listed first so it is the one
# that survives a tie, since it needs no pinning and so reuses the plain build.
#
SWEEP_SET=${SWEEP_SET:-featured}
GEOMETRIES=(32x32 64x64)          # square arrays; add 32x64 64x32 for asymmetric
BASELINE_GEOMETRY=${BASELINE_GEOMETRY:-32x32}
INCLUDE_SYSTOLIC=1
read -r -a PORT_WIDTHS <<< "${PORT_WIDTHS:-matched baseline}"
if [ "$SWEEP_SET" = featured ]; then
  # One native point per array size: 8-bit cells, CH_OUT=8, CH_IN=K
  CELL_BITS=(8); CH_OUT_VALUES=(8); CH_IN_DEFAULT=geom; CH_IN_EXTRA=()
else
  CELL_BITS=(8 4); CH_OUT_VALUES=(8 16); CH_IN_DEFAULT=geom; CH_IN_EXTRA=(64x64:32)
fi
# =============================================================================

set +u
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1   # repo root
source .envrc >/dev/null 2>&1          # env.sh hard-exits under set -u
export PYTHONPATH="$PWD/voyager-compiler/src${PYTHONPATH:+:$PYTHONPATH}"
ulimit -s unlimited                    # 64x64 Harness is a ~33 MB stack object

RES=$1
shift
if [ -z "$RES" ]; then
  sed -n '2,15p' "${BASH_SOURCE[0]}"
  exit 2
fi
mkdir -p "$RES"

export DATATYPE=INT8 INPUT_BUFFER_SIZE=1024 WEIGHT_BUFFER_SIZE=1024 ACCUM_BUFFER_SIZE=1024
export CLOCK_PERIOD=5 NETWORK=mobilebert_encoder SIMS=gold,accelerator
export CODEGEN_DIR=${CODEGEN_DIR:-test/compiler}
DTYPE_BITS=8                           # INT8 operands on both ports

LAYERS=(
  mobilebert_encoder_layer_0_ffn_0_output_dense_fused        # M=128 K=512 N=128
  mobilebert_encoder_layer_0_output_bottleneck_dense_fused   # M=128 K=128 N=512
  mobilebert_encoder_layer_0_attention_output_dense_fused    # M=512 K=128 N=128
  matmul_6_fused                                             # M=128 K=128 N=32, act x act
  matmul_2_fused                                             # M=128 K=32  N=128, act x act
)

# --- Expand the design space into concrete config records --------------------
# Each record: name|backend|K|N|cell|ch_in|ch_out|IAT|OAT
# Derivation (TILE_INPUT/OUTPUT_AXIS_ELEMENTS pinned to 1, throughput-neutral):
#   N_per_element = CH_OUT * cell / 8   (= CH_OUT for 8b, CH_OUT/2 for 4b)
#   OUTPUT_AXIS_TILES = N / N_per_element      INPUT_AXIS_TILES = K / CH_IN
# A record is dropped (with a warning) if the geometry is not tileable by the
# chosen macro, so an over-broad design space stays safe to run.
CONFIGS=()
emit_systolic() { CONFIGS+=("sa_$1x$2|0|$1|$2|||||"); }
emit_cim() {   # $1=K $2=N $3=cell $4=ch_in $5=ch_out
  local K=$1 N=$2 cell=$3 chin=$4 chout=$5
  if (( chout * cell % 8 != 0 )); then echo "  skip cim ${K}x${N} b${cell} co${chout}: cell/CH_OUT mismatch" >&2; return; fi
  local nper=$(( chout * cell / 8 ))
  if (( N % nper != 0 || K % chin != 0 )); then
    echo "  skip cim ${K}x${N} b${cell} ci${chin} co${chout}: not tileable" >&2; return; fi
  local oat=$(( N / nper )) iat=$(( K / chin ))
  # Name encodes both macro operand widths (A and B); they are equal in this sweep
  CONFIGS+=("cim${cell}b${cell}b_${K}x${N}_co${chout}_ci${chin}|1|$K|$N|$cell|$chin|$chout|$iat|$oat")
}
for geom in "${GEOMETRIES[@]}"; do
  K=${geom%x*}; N=${geom#*x}
  [ "$INCLUDE_SYSTOLIC" = 1 ] && emit_systolic "$K" "$N"
  for cell in "${CELL_BITS[@]}"; do
    for chout in "${CH_OUT_VALUES[@]}"; do
      [ "$CH_IN_DEFAULT" = geom ] && chin=$K || chin=$CH_IN_DEFAULT
      emit_cim "$K" "$N" "$cell" "$chin" "$chout"
      for extra in "${CH_IN_EXTRA[@]}"; do
        [ "${extra%%:*}" = "$geom" ] && emit_cim "$K" "$N" "$cell" "${extra#*:}" "$chout"
      done
    done
  done
done

# --- Port-width axis ---------------------------------------------------------
BASE_K=${BASELINE_GEOMETRY%x*}; BASE_N=${BASELINE_GEOMETRY#*x}
BASE_IC_BITS=$(( BASE_K * DTYPE_BITS )); BASE_OC_BITS=$(( BASE_N * DTYPE_BITS ))

# Resolves a token to the "<ic_bits> <oc_bits>" it pins. "matched" resolves to
# the design's own width, which is what the architecture derives unaided, so it
# is reported as a width but left unpinned at build time.
port_bits_for() {   # $1=token $2=K $3=N
  case $1 in
    baseline) echo "$BASE_IC_BITS $BASE_OC_BITS" ;;
    matched)  echo "$(( $2 * DTYPE_BITS )) $(( $3 * DTYPE_BITS ))" ;;
    *)        echo "$1 $1" ;;
  esac
}

# Tokens to run for one geometry, dropping any that lands on the same widths as
# another (a design the size of the baseline has baseline == matched).
port_widths_for() {   # $1=K $2=N -> tokens on stdout
  local tok
  for tok in "${PORT_WIDTHS[@]}"; do
    echo "$(port_bits_for "$tok" "$1" "$2")|$tok"
  done | awk -F'|' '!seen[$1]++ {print $2}'
}

# --- Manifest: single source of config metadata for the parser ---------------
# ic/oc_matched_port_bits let the parser size peak external bandwidth without
# re-deriving the datatype width.
MANIFEST="$RES/manifest.csv"
echo "config,backend,K,N,cell_bits,ch_in,ch_out,input_axis_tiles,output_axis_tiles,ic_matched_port_bits,oc_matched_port_bits,port_widths" > "$MANIFEST"
for rec in "${CONFIGS[@]}"; do
  IFS='|' read -r _ _ K N _ _ _ _ _ <<< "$rec"
  echo "$(echo "$rec" | tr '|' ','),$(( K * DTYPE_BITS )),$(( N * DTYPE_BITS )),$(port_widths_for "$K" "$N" | paste -sd' ' -)"
done >> "$MANIFEST"

if [ "${DRY_RUN:-0}" = 1 ]; then
  runs=0
  echo "Design space -> ${#CONFIGS[@]} configs (baseline $BASELINE_GEOMETRY = ${BASE_IC_BITS}/${BASE_OC_BITS} b):"
  for rec in "${CONFIGS[@]}"; do
    IFS='|' read -r name _ K N _ _ _ _ _ <<< "$rec"
    ports=""
    for tok in $(port_widths_for "$K" "$N"); do
      read -r icb ocb <<< "$(port_bits_for "$tok" "$K" "$N")"
      ports+="${ports:+, }${tok} $((icb/8))/$((ocb/8))B"
      runs=$(( runs + 1 ))
    done
    printf '%s\tports: %s\n' "$(tr '|' '\t' <<< "$rec")" "$ports"
  done
  echo "$runs builds total; manifest written to $MANIFEST"
  exit 0
fi

WANT=("$@")
want() { [ ${#WANT[@]} -eq 0 ] && return 0; for w in "${WANT[@]}"; do [ "$w" = "$1" ] && return 0; done; return 1; }

for rec in "${CONFIGS[@]}"; do
  IFS='|' read -r name backend K N cell chin chout iat oat <<< "$rec"
  want "$name" || continue
  # Port width is the inner axis: same design, different external interface.
  # Body left at this indent level; `continue` skips one (config, port) build.
  for tok in $(port_widths_for "$K" "$N"); do
  read -r icb ocb <<< "$(port_bits_for "$tok" "$K" "$N")"
  if [ "$tok" = matched ]; then
    # Leaving the widths unset is exactly the matched case, and it keeps the
    # build directory free of a port signature
    unset IC_PORT_WIDTH OC_PORT_WIDTH
    SUFFIX=""
  else
    export IC_PORT_WIDTH=$icb OC_PORT_WIDTH=$ocb
    if [ "$icb" = "$ocb" ]; then SUFFIX="_pw${icb}"; else SUFFIX="_pw${icb}x${ocb}"; fi
  fi
  echo "=== [$(date +%T)] $name  port ${tok}: ${icb}/${ocb} b = $((icb/8))/$((ocb/8)) B/cyc ==="
  export MATRIX_BACKEND=$backend IC_DIMENSION=$K OC_DIMENSION=$N
  if [ "$backend" = 1 ]; then
    local_basec=20; [ "$cell" = 8 ] && local_basec=24
    export CIM_CH_IN=$chin CIM_CH_OUT=$chout CIM_B_SETS=2 \
           CIM_BASE_A_WIDTH=$cell CIM_BASE_B_WIDTH=$cell CIM_BASE_C_WIDTH=$local_basec \
           CIM_WRITE_CH_IN=1 CIM_MAC_LATENCY=1 CIM_MODE=0 \
           CIM_TILE_INPUT_AXIS_ELEMENTS=1 CIM_TILE_OUTPUT_AXIS_ELEMENTS=1 \
           CIM_INPUT_AXIS_TILES=$iat CIM_OUTPUT_AXIS_TILES=$oat \
           CIM_A_PORT_TILES=$iat CIM_B_PORT_TILES=$oat CIM_C_PORT_TILES=$oat \
           CIM_C_BEAT_LAYOUT=1
  fi

  tag="${name}${SUFFIX}"
  make network-proto > "$RES/${tag}_proto.log" 2>&1 || { echo "  PROTO FAILED"; continue; }
  make -j32 TestRunner > "$RES/${tag}_build.log" 2>&1 || {
    echo "  BUILD FAILED"; tail -20 "$RES/${tag}_build.log"; continue; }
  # SystemC pass first: fast functional check against the gold model
  for layer in "${LAYERS[@]}"; do
    ( TESTS=$layer timeout 7200 make sim > "$RES/${tag}__${layer}.log" 2>&1 ) &
  done
  wait
  fails=$(grep -L "Error count: 0" "$RES/${tag}__"*.log | wc -l)
  echo "  systemc done $(date +%T), gold failures: $fails"

  # RTL pass (default): Catapult synthesis, then VCS cosim for cycle-accurate
  # runtime and the hardware performance counters. RTL=0 skips it (smoke check).
  if [ "${RTL:-1}" = 1 ]; then
    if [ "$fails" != 0 ]; then echo "  SKIP RTL: gold failures"; continue; fi
    export TECHNOLOGY=generic ENABLE_PERF_COUNTERS=1
    if ! make -j16 rtl > "$RES/${tag}_rtl_gen.log" 2>&1; then
      echo "  RTL GEN FAILED"; tail -5 "$RES/${tag}_rtl_gen.log"; continue
    fi
    echo "  rtl generated $(date +%T)"
    joined=$(IFS=,; echo "${LAYERS[*]}")
    python run_regression.py --models mobilebert_encoder --sims rtl --keep_build \
      --num_processes ${#LAYERS[@]} --tests "$joined" > "$RES/${tag}_rtl_run.log" 2>&1
    latest=$(ls -td regression_results/*/ | head -1)
    for layer in "${LAYERS[@]}"; do
      cp "${latest}mobilebert_encoder_${layer}.log" "$RES/${tag}__rtl__${layer}.log" 2>/dev/null
    done
    echo "  rtl sims done $(date +%T)"
  fi
  done   # port width
done
echo "ALL DONE $(date +%T)"
