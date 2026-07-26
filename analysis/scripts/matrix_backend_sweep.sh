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
# A CIM design point is fully described by (array K x N, macro native width,
# macro CH_IN, macro CH_OUT, CIMTile organization); the tiling to reach K x N is
# derived below, so only these knobs need touching.
#
#   SWEEP_SET   featured | full   (env-overridable, default featured)
#     featured  one native CIM point per array size, plus the systolic baseline
#               -- the headline comparison, cheap enough for the RTL flow
#     full      the complete cross of the axis arrays below
#
#   GEOMETRIES     target array dimensions, "K x N" (= IC x OC of the layer map)
#   MACRO_NATIVE_WIDTHS  CIM macro native operand width in bits; 8 matches the
#                  INT8 datapath (the native point), 4 is the vanilla sub-word
#                  macro (two cells per INT8 weight, two A slices per MAC)
#   CH_OUT_VALUES  macro output channels; N is reached by tiling these
#   CH_IN_DEFAULT  macro input channels unless an override applies; K is reached
#                  by tiling these (K / CH_IN input-axis tiles). "geom" => K
#   CH_IN_EXTRA    extra CH_IN constructions for specific geometries,
#                  "KxN:CH_IN" (adds points; does not replace the default)
#   TILE_ORGS      CIMTile organization, "<in>x<out>" elements per tile. The
#                  array hierarchy is CIMArray > CIMTile > CIMElement > macro,
#                  and a geometry is reached as
#                      K = INPUT_AXIS_TILES  * TILE_IN_ELEMS  * CH_IN
#                      N = OUTPUT_AXIS_TILES * TILE_OUT_ELEMS * N_per_element
#                  so the same K x N can be built as many tiles of few elements
#                  or few tiles of many. Throughput-neutral at fixed K x N (the
#                  split changes no port width while B_PORT_TILES = OAT), which
#                  is why 1x1 is the default -- but it is a real organizational
#                  axis and is recorded per design point, never assumed.
#   INCLUDE_SYSTOLIC  1 to also emit the systolic baseline for each geometry
#
#   BASELINE_GEOMETRY  the systolic array every design point is judged against.
#                  Its dimensions set the "baseline" port width below, so the
#                  constant-bandwidth point follows the baseline automatically
#                  instead of being a magic number.
#
# The interface model is one word per cycle per port (width/8 B/cyc each for
# input, weight, bias and output). The width axis applies to CIM points ONLY:
#
#   the systolic array is always at its own dimension -- sa_32x32 at 256 b,
#   sa_64x64 at 512 b. It is the baseline, and a baseline only means anything
#   fed one row/column per cycle; it is never pinned to another array's width.
#
#   a CIM array is run at BOTH:
#     baseline  the BASELINE array's width (BASELINE_GEOMETRY 32x32 at INT8 =
#               256 b = 32 B/cyc). The array grows while the external interface
#               stays exactly what the baseline had -- the drop-in-replacement
#               question, rated against the baseline array itself.
#     matched   its own dimension (a 64x64 INT8 array gets 512 b = 64 B/cyc),
#               what the array can actually absorb, and exactly what
#               ArchitectureParams.h derives when the widths are left unset --
#               the same-geometry question, rated against the systolic array of
#               that same K x N.
#
# So against a 32x32 systolic baseline, a 64x64 CIM array is built twice, at
# 256 b and at 512 b, and each build answers to a different systolic row. The
# two coincide for a CIM design the size of the baseline (a 32x32 CIM point is
# 256 b either way) and that duplicate is dropped rather than built twice --
# "matched" is listed first so it is the one that survives a tie, since it needs
# no pinning and so reuses the plain build directory.
#
# Narrowing PORT_WIDTHS to a single token is a deliberate act: it produces a
# partial dataset. The banner printed at startup states the widths in effect,
# and a CIM point whose rating baseline is missing is warned about by name.
#
SWEEP_SET=${SWEEP_SET:-featured}
GEOMETRIES=(32x32 64x64)          # square arrays; add 32x64 64x32 for asymmetric
BASELINE_GEOMETRY=${BASELINE_GEOMETRY:-32x32}
INCLUDE_SYSTOLIC=1
read -r -a PORT_WIDTHS <<< "${PORT_WIDTHS:-matched baseline}"
read -r -a TILE_ORGS <<< "${TILE_ORGS:-1x1}"   # elements per CIMTile, "<in>x<out>"
if [ "$SWEEP_SET" = featured ]; then
  # One native point per array size: 8b native width, CH_OUT=8, CH_IN=K
  MACRO_NATIVE_WIDTHS=(8); CH_OUT_VALUES=(8); CH_IN_DEFAULT=geom; CH_IN_EXTRA=()
else
  MACRO_NATIVE_WIDTHS=(8 4); CH_OUT_VALUES=(8 16); CH_IN_DEFAULT=geom; CH_IN_EXTRA=(64x64:32)
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
# Each record: name|backend|K|N|native|ch_in|ch_out|IAT|OAT|TILE_IN_ELEMS|TILE_OUT_ELEMS
# Derivation, walking the hierarchy CIMArray > CIMTile > CIMElement > macro:
#   N_per_element = CH_OUT * native / 8   (= CH_OUT for 8b, CH_OUT/2 for 4b)
#   OUTPUT_AXIS_TILES = N / (TILE_OUT_ELEMS * N_per_element)
#   INPUT_AXIS_TILES  = K / (TILE_IN_ELEMS  * CH_IN)
# A record is dropped (with a warning) if the geometry is not tileable by the
# chosen macro and tile organization, so an over-broad design space stays safe
# to run.
CONFIGS=()
emit_systolic() { CONFIGS+=("sa_$1x$2|0|$1|$2|||||||"); }
emit_cim() {   # $1=K $2=N $3=native $4=ch_in $5=ch_out $6=tile_org
  local K=$1 N=$2 native=$3 chin=$4 chout=$5 org=$6
  local tie=${org%x*} toe=${org#*x}
  if (( chout * native % 8 != 0 )); then echo "  skip cim ${K}x${N} b${native} co${chout}: native width/CH_OUT mismatch" >&2; return; fi
  local nper=$(( chout * native / 8 ))
  local ntile=$(( toe * nper )) ktile=$(( tie * chin ))
  if (( N % ntile != 0 || K % ktile != 0 )); then
    echo "  skip cim ${K}x${N} b${native} ci${chin} co${chout} t${org}: not tileable" >&2; return; fi
  local oat=$(( N / ntile )) iat=$(( K / ktile ))
  # Name encodes both macro operand widths (A and B), equal in this sweep, and
  # the tile organization when it is not the default one element per tile
  local name="cim${native}b${native}b_${K}x${N}_co${chout}_ci${chin}"
  [ "$org" = 1x1 ] || name+="_t${tie}x${toe}"
  CONFIGS+=("$name|1|$K|$N|$native|$chin|$chout|$iat|$oat|$tie|$toe")
}
for geom in "${GEOMETRIES[@]}"; do
  K=${geom%x*}; N=${geom#*x}
  [ "$INCLUDE_SYSTOLIC" = 1 ] && emit_systolic "$K" "$N"
  for native in "${MACRO_NATIVE_WIDTHS[@]}"; do
    for chout in "${CH_OUT_VALUES[@]}"; do
      for org in "${TILE_ORGS[@]}"; do
        [ "$CH_IN_DEFAULT" = geom ] && chin=$K || chin=$CH_IN_DEFAULT
        emit_cim "$K" "$N" "$native" "$chin" "$chout" "$org"
        for extra in "${CH_IN_EXTRA[@]}"; do
          [ "${extra%%:*}" = "$geom" ] && emit_cim "$K" "$N" "$native" "${extra#*:}" "$chout" "$org"
        done
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

# Tokens to run for one design point, dropping any that lands on the same widths
# as another (a design the size of the baseline has baseline == matched).
#
# The width axis is CIM-only. The systolic array IS the baseline, and a baseline
# is only meaningful at its own dimension: sa_32x32 runs at 256 b and sa_64x64
# at 512 b, each feeding its array one row/column per cycle, and neither is ever
# pinned to the other's width. A CIM array is what gets seen twice -- once
# behind the baseline's interface (the drop-in-replacement question, rated
# against the baseline array) and once at the width its own dimension calls for
# (the same-geometry question, rated against the systolic array of that size).
port_widths_for() {   # $1=backend $2=K $3=N -> tokens on stdout
  if [ "$1" != 1 ]; then echo matched; return; fi
  local tok
  for tok in "${PORT_WIDTHS[@]}"; do
    echo "$(port_bits_for "$tok" "$2" "$3")|$tok"
  done | awk -F'|' '!seen[$1]++ {print $2}'
}

# --- Manifest: single source of config metadata for the parser ---------------
# ic/oc_matched_port_bits let the parser size peak external bandwidth without
# re-deriving the datatype width. port_widths records the resolved width beside
# its token ("512:matched 256:baseline") so a reader never has to re-derive
# which interfaces a design point covers.
MANIFEST="$RES/manifest.csv"
# baseline_geometry names the array every point is judged against, so the
# workbook can pair a baseline-width CIM point with the baseline array itself
# instead of guessing which systolic config that is. port_widths stays last:
# the merge below keys on it as the final field.
MANIFEST_HEADER="config,backend,K,N,macro_native_width_bits,ch_in,ch_out,input_axis_tiles,output_axis_tiles,tile_input_axis_elements,tile_output_axis_elements,ic_matched_port_bits,oc_matched_port_bits,baseline_geometry,port_widths"
manifest_rows() {
  local rec backend K N tok icb ocb ports
  for rec in "${CONFIGS[@]}"; do
    IFS='|' read -r _ backend K N _ _ _ _ _ _ _ <<< "$rec"
    ports=""
    for tok in $(port_widths_for "$backend" "$K" "$N"); do
      read -r icb ocb <<< "$(port_bits_for "$tok" "$K" "$N")"
      [ "$icb" = "$ocb" ] && ports+="${ports:+ }${icb}:${tok}" \
                          || ports+="${ports:+ }${icb}x${ocb}:${tok}"
    done
    echo "$(echo "$rec" | tr '|' ','),$(( K * DTYPE_BITS )),$(( N * DTYPE_BITS )),$BASELINE_GEOMETRY,$ports"
  done
}

# A results directory accumulates across invocations -- a resumed run, or one
# narrowed to a config subset or a single PORT_WIDTHS token, adds to logs that
# are already there. So the manifest is MERGED, never clobbered: rows for
# configs this invocation does not build are kept, and port_widths carries the
# union across invocations. Clobbering would leave the manifest describing only
# the last invocation while the directory holds every invocation's logs, which
# is exactly how a manifest comes to claim a port width nothing ever ran at.
if [ -f "$MANIFEST" ] && [ "$(head -1 "$MANIFEST")" != "$MANIFEST_HEADER" ]; then
  echo "note: $MANIFEST predates the current schema; kept as manifest.csv.bak" >&2
  mv "$MANIFEST" "$MANIFEST.bak"
fi
if [ -f "$MANIFEST" ]; then
  { echo "$MANIFEST_HEADER"
    awk -F, -v OFS=, '
      FNR==NR { if (FNR > 1) { old[$1] = $0; oldpw[$1] = $NF } ; next }
      {
        if ($1 in oldpw) {
          # Union the port widths, keyed on the resolved width rather than the
          # token: for a baseline-sized design "matched" and "baseline" are the
          # same build, and must not accumulate as two entries.
          n = split(oldpw[$1] " " $NF, a, " "); pw = ""; delete seen
          for (i = 1; i <= n; i++) {
            if (a[i] == "") continue
            split(a[i], p, ":")
            if (p[1] in seen) continue
            seen[p[1]] = 1; pw = pw (pw == "" ? "" : " ") a[i]
          }
          $NF = pw
        }
        print; built[$1] = 1
      }
      END { for (c in old) if (!(c in built)) print old[c] }   # configs not built now
    ' "$MANIFEST" <(manifest_rows)
  } > "$MANIFEST.tmp" && mv "$MANIFEST.tmp" "$MANIFEST"
else
  { echo "$MANIFEST_HEADER"; manifest_rows; } > "$MANIFEST"
fi

if [ "${DRY_RUN:-0}" = 1 ]; then
  runs=0
  echo "Design space -> ${#CONFIGS[@]} configs (baseline $BASELINE_GEOMETRY = ${BASE_IC_BITS}/${BASE_OC_BITS} b):"
  for rec in "${CONFIGS[@]}"; do
    IFS='|' read -r name backend K N _ _ _ _ _ _ _ <<< "$rec"
    ports=""
    for tok in $(port_widths_for "$backend" "$K" "$N"); do
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

# State the swept axes up front: a narrowed PORT_WIDTHS or config subset yields
# a partial dataset, and that has to be visible in the run log, not inferred
# afterwards from which files happen to exist.
echo "port widths: ${PORT_WIDTHS[*]}  (baseline $BASELINE_GEOMETRY = ${BASE_IC_BITS}/${BASE_OC_BITS} b)"
echo "tile orgs:   ${TILE_ORGS[*]} elements per CIMTile (in x out)"
[ ${#WANT[@]} -gt 0 ] && echo "configs:     ${WANT[*]} (subset of ${#CONFIGS[@]})"

# Each CIM width is rated against a different systolic array, and both of those
# arrays have to exist for the point to mean anything:
#   matched  -> the systolic array of the SAME K x N, at its own width
#   baseline -> the BASELINE array itself, at its own width
# Both are unpinned "matched" builds, so this only has to look for the plain
# config name. A config subset can easily leave one of them unbuilt -- the run
# still succeeds and the gap surfaces only later, as a workbook block with no
# baseline row. Catch it here, while it is still cheap to fix.
have_systolic() {   # $1=config name -> is it being built now, or already on disk?
  want "$1" && return 0
  compgen -G "$RES/$1__*.log" >/dev/null
}
for rec in "${CONFIGS[@]}"; do
  IFS='|' read -r name backend K N _ _ _ _ _ _ _ <<< "$rec"
  [ "$backend" = 1 ] || continue
  want "$name" || continue
  for tok in $(port_widths_for "$backend" "$K" "$N"); do
    case $tok in
      matched)  sa_name="sa_${K}x${N}" ;;
      baseline) sa_name="sa_${BASELINE_GEOMETRY}" ;;
      *)        continue ;;                  # an explicit width rates against nothing
    esac
    have_systolic "$sa_name" && continue
    read -r icb ocb <<< "$(port_bits_for "$tok" "$K" "$N")"
    echo "WARNING: $name at ${icb}/${ocb} b ($tok) will have no baseline --" \
         "$sa_name is neither requested nor already present in $RES" >&2
  done
done

for rec in "${CONFIGS[@]}"; do
  IFS='|' read -r name backend K N native chin chout iat oat tie toe <<< "$rec"
  want "$name" || continue
  # Port width is the inner axis: same design, different external interface.
  # Body left at this indent level; `continue` skips one (config, port) build.
  for tok in $(port_widths_for "$backend" "$K" "$N"); do
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
    local_basec=20; [ "$native" = 8 ] && local_basec=24
    export CIM_CH_IN=$chin CIM_CH_OUT=$chout CIM_B_SETS=2 \
           CIM_BASE_A_WIDTH=$native CIM_BASE_B_WIDTH=$native CIM_BASE_C_WIDTH=$local_basec \
           CIM_WRITE_CH_IN=1 CIM_MAC_LATENCY=1 CIM_MODE=0 \
           CIM_TILE_INPUT_AXIS_ELEMENTS=$tie CIM_TILE_OUTPUT_AXIS_ELEMENTS=$toe \
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
