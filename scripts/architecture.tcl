if { $DATATYPE == "P8_1" } {
  set INPUT_DATATYPE "DataTypes::posit8"
  set WEIGHT_DATATYPE "DataTypes::posit8"
  set ACCUM_DATATYPE "DataTypes::bfloat16"
  set VECTOR_DATATYPE "DataTypes::bfloat16"

  set SA_INPUT_TYPE "Posit<8, 1>::decoded"
  set SA_WEIGHT_TYPE "Posit<8, 1>::decoded"

  set ACC_BUF_C_DATA_REP_NAME "float_val.d"

  set INPUT_DTYPE_WIDTH 8
  set WEIGHT_DTYPE_WIDTH 8
  set ACCUM_DATATYPE_WIDTH 16
  set CIM_SIGNED true
} elseif { $DATATYPE == "E4M3" } {
  set INPUT_DATATYPE "DataTypes::e4m3"
  set WEIGHT_DATATYPE "DataTypes::e4m3"
  set ACCUM_DATATYPE "DataTypes::bfloat16"
  set VECTOR_DATATYPE "DataTypes::bfloat16"

  set ACC_BUF_C_DATA_REP_NAME "float_val.d"

  set INPUT_DTYPE_WIDTH 8
  set WEIGHT_DTYPE_WIDTH 8
  set ACCUM_DATATYPE_WIDTH 16
  set CIM_SIGNED true
} elseif { $DATATYPE == "E4M3_NS" } {
  set INPUT_DATATYPE "F8"
  set WEIGHT_DATATYPE "F8"
  set ACCUM_DATATYPE "F16"
  set VECTOR_DATATYPE "F16"

  set ACC_BUF_C_DATA_REP_NAME "float_val.d"

  set INPUT_DTYPE_WIDTH 8
  set WEIGHT_DTYPE_WIDTH 8
  set ACCUM_DATATYPE_WIDTH 16
  set CIM_SIGNED true
} elseif { $DATATYPE == "E4M3_DW" } {
  set INPUT_DATATYPE "F8"
  set WEIGHT_DATATYPE "F8"
  set ACCUM_DATATYPE "F16"
  set VECTOR_DATATYPE "F16"

  set ACC_BUF_C_DATA_REP_NAME "float_val.d"

  set INPUT_DTYPE_WIDTH 8
  set WEIGHT_DTYPE_WIDTH 8
  set ACCUM_DATATYPE_WIDTH 16
  set CIM_SIGNED true
} elseif { $DATATYPE == "E4M3_DW_NS" } {
  set INPUT_DATATYPE "F8"
  set WEIGHT_DATATYPE "F8"
  set ACCUM_DATATYPE "F16"
  set VECTOR_DATATYPE "F16"

  set ACC_BUF_C_DATA_REP_NAME "float_val.d"

  set INPUT_DTYPE_WIDTH 8
  set WEIGHT_DTYPE_WIDTH 8
  set ACCUM_DATATYPE_WIDTH 16
  set CIM_SIGNED true
} elseif { $DATATYPE == "E5M2" } {
  set INPUT_DATATYPE "DataTypes::e5m2"
  set WEIGHT_DATATYPE "DataTypes::e5m2"
  set ACCUM_DATATYPE "DataTypes::bfloat16"
  set VECTOR_DATATYPE "DataTypes::bfloat16"

  set ACC_BUF_C_DATA_REP_NAME "float_val.d"

  set INPUT_DTYPE_WIDTH 8
  set WEIGHT_DTYPE_WIDTH 8
  set ACCUM_DATATYPE_WIDTH 16
  set CIM_SIGNED true
} elseif { $DATATYPE == "HYBRID_FP8" } {
  set INPUT_DATATYPE "F8"
  set WEIGHT_DATATYPE "F8"
  set ACCUM_DATATYPE "F16"
  set VECTOR_DATATYPE "F16"

  set ACC_BUF_C_DATA_REP_NAME "float_val.d"

  set INPUT_DTYPE_WIDTH 8
  set WEIGHT_DTYPE_WIDTH 8
  set ACCUM_DATATYPE_WIDTH 16
  set CIM_SIGNED true
} elseif { $DATATYPE == "BF16" } {
  set INPUT_DATATYPE "DataTypes::bfloat16"
  set WEIGHT_DATATYPE "DataTypes::bfloat16"
  set ACCUM_DATATYPE "DataTypes::bfloat16"
  set VECTOR_DATATYPE "DataTypes::bfloat16"

  set ACC_BUF_C_DATA_REP_NAME "float_val.d"

  set IO_DATATYPE_WIDTH 16
  set ACCUM_DATATYPE_WIDTH 16
  set CIM_SIGNED true
} elseif { $DATATYPE == "FP32" } {
  set INPUT_DATATYPE "DataTypes::float32"
  set WEIGHT_DATATYPE "DataTypes::float32"
  set ACCUM_DATATYPE "DataTypes::float32"
  set VECTOR_DATATYPE "DataTypes::float32"

  set ACC_BUF_C_DATA_REP_NAME "float_val.d"

  set INPUT_DTYPE_WIDTH 32
  set WEIGHT_DTYPE_WIDTH 32
  set ACCUM_DATATYPE_WIDTH 32
  set CIM_SIGNED true
} elseif { $DATATYPE == "INT8" } {
  set INPUT_DATATYPE "DataTypes::int8"
  set WEIGHT_DATATYPE "DataTypes::int8"
  set ACCUM_DATATYPE "DataTypes::int24"
  set VECTOR_DATATYPE "DataTypes::bfloat16"

  set ACC_BUF_C_DATA_REP_NAME "int_val"

  set INPUT_DTYPE_WIDTH 8
  set WEIGHT_DTYPE_WIDTH 8
  set ACCUM_DATATYPE_WIDTH 24
  set CIM_SIGNED true
} elseif { $DATATYPE == "INT8_32" } {
  set INPUT_DATATYPE "DataTypes::int8"
  set WEIGHT_DATATYPE "DataTypes::int8"
  set ACCUM_DATATYPE "DataTypes::int32"
  set VECTOR_DATATYPE "DataTypes::bfloat16"

  set ACC_BUF_C_DATA_REP_NAME "int_val"

  set INPUT_DTYPE_WIDTH 8
  set WEIGHT_DTYPE_WIDTH 8
  set ACCUM_DATATYPE_WIDTH 32
  set CIM_SIGNED true
} elseif {$DATATYPE == "MXINT8"} {
  set INPUT_DATATYPE "DataTypes::int8"
  set WEIGHT_DATATYPE "DataTypes::int8"
  set ACCUM_DATATYPE "DataTypes::int32"
  set ACCUM_BUFFER_DATATYPE "DataTypes::bfloat16"
  set VECTOR_DATATYPE "DataTypes::bfloat16"
  set SCALE_DATATYPE "DataTypes::fp8_e8m0"

  set ACC_BUF_C_DATA_REP_NAME "float_val.d"

  set SUPPORT_MX true

  set INPUT_DTYPE_WIDTH 8
  set WEIGHT_DTYPE_WIDTH 8
  set ACCUM_DATATYPE_WIDTH 16
  set SCALE_DATATYPE_WIDTH 8
  set CIM_SIGNED true
} elseif {$DATATYPE == "MXNF4"} {
  set INPUT_DATATYPE "DataTypes::uint2, DataTypes::int4, DataTypes::int6"
  set WEIGHT_DATATYPE "DataTypes::uint2, DataTypes::int4, DataTypes::int6"
  set ACCUM_DATATYPE "DataTypes::int18"
  set ACCUM_BUFFER_DATATYPE "DataTypes::bfloat16"
  set VECTOR_DATATYPE "DataTypes::bfloat16"
  set SCALE_DATATYPE "DataTypes::fp8_e5m3"
  set VU_INPUT_TYPES "DataTypes::bfloat16, DataTypes::fp8_e5m3, DataTypes::e4m3, DataTypes::int1"
  set OUTPUT_DATATYPES "$INPUT_DATATYPE, DataTypes::bfloat16, DataTypes::fp8_e5m3, DataTypes::e4m3"
  set SPMM_META_DATATYPE "DataTypes::int32"

  set SA_INPUT_TYPE "DataTypes::int6"
  set SA_WEIGHT_TYPE "DataTypes::int6"

  set SUPPORT_MX true

  set ACC_BUF_C_DATA_REP_NAME "float_val.d"

  set INPUT_DTYPE_WIDTH 6
  set WEIGHT_DTYPE_WIDTH 6
  set ACCUM_DATATYPE_WIDTH 16
  set SCALE_DATATYPE_WIDTH 8

  set IC_PORT_WIDTH [expr {$IC_DIMENSION * 4}]
  set OC_PORT_WIDTH [expr {$OC_DIMENSION * 4}]
  set MV_UNIT_WIDTH [expr {$OC_DIMENSION * 2}]
  set SPMM_UNIT_WIDTH $OC_DIMENSION

  set VECTOR_UNIT_WIDTH $OC_DIMENSION
  set REDUCER_WIDTH [expr {$OC_DIMENSION / 2}]
  set ACCUMULATOR_WIDTH [expr {$OC_DIMENSION / 2}]
  set CIM_SIGNED true
} else {
  puts "Invalid DATATYPE"
  exit 1
}

if {![info exists SUPPORT_MX]} {
  set SUPPORT_MX false
}

if {![info exists SUPPORT_MVM]} {
  set SUPPORT_MVM false
}

if {![info exists SUPPORT_SPMM]} {
  set SUPPORT_SPMM false
}

if {![info exists SUPPORT_DWC]} {
  set SUPPORT_DWC false
}

set MATRIX_BACKEND_SYSTOLIC 0
set MATRIX_BACKEND_CIM 1

if {![info exists MATRIX_BACKEND]} {
  set MATRIX_BACKEND $MATRIX_BACKEND_SYSTOLIC
}

if {$MATRIX_BACKEND != $MATRIX_BACKEND_SYSTOLIC &&
    $MATRIX_BACKEND != $MATRIX_BACKEND_CIM} {
  error "MATRIX_BACKEND must be 0 (systolic) or 1 (CIM)"
}

if {![info exists VECTOR_UNIT_WIDTH]} {
  set VECTOR_UNIT_WIDTH $OC_DIMENSION
}

if {![info exists REDUCER_WIDTH]} {
  set REDUCER_WIDTH $OC_DIMENSION
}

if {![info exists ACCUMULATOR_WIDTH]} {
  set ACCUMULATOR_WIDTH $OC_DIMENSION
}

# ================================================================
# Default Datatypes
# ================================================================

if {![info exists SA_INPUT_TYPE]} {
  set SA_INPUT_TYPE $INPUT_DATATYPE
}

if {![info exists SA_WEIGHT_TYPE]} {
  set SA_WEIGHT_TYPE $WEIGHT_DATATYPE
}

if {![info exists ACCUM_BUFFER_DATATYPE]} {
  set ACCUM_BUFFER_DATATYPE $ACCUM_DATATYPE
}

if {![info exists SCALE_DATATYPE]} {
  set SCALE_DATATYPE "DataTypes::fp8_e8m0"
}

if {![info exists VU_INPUT_TYPES]} {
  if {$SUPPORT_MX} {
    set VU_INPUT_TYPES "$VECTOR_DATATYPE, $SCALE_DATATYPE"
  } else {
    set VU_INPUT_TYPES "$INPUT_DATATYPE, $VECTOR_DATATYPE"
  }
}

if {![info exists OUTPUT_DATATYPES]} {
  if {$SUPPORT_MX} {
    set OUTPUT_DATATYPES "$INPUT_DATATYPE, $VECTOR_DATATYPE, $SCALE_DATATYPE"
  } else {
    set OUTPUT_DATATYPES "$INPUT_DATATYPE, $VECTOR_DATATYPE"
  }
}

# ================================================================
# Datatype Width Configuration
# ================================================================

set INPUT_TYPE_LIST "std::tuple<$INPUT_DATATYPE>"
set WEIGHT_TYPE_LIST "std::tuple<$WEIGHT_DATATYPE>"

if {![info exists INPUT_DTYPE_WIDTH]} {
  set INPUT_DTYPE_WIDTH "${INPUT_DATATYPE}::width"
}

if {![info exists WEIGHT_DTYPE_WIDTH]} {
  set WEIGHT_DTYPE_WIDTH "${WEIGHT_DATATYPE}::width"
}

# ================================================================
# CIM Configuration
# ================================================================

if {![info exists CIM_CH_IN]} {
  set CIM_CH_IN 64
}

if {![info exists CIM_CH_OUT]} {
  set CIM_CH_OUT 8
}

if {![info exists CIM_B_SETS]} {
  set CIM_B_SETS 18
}

if {![info exists CIM_BASE_A_WIDTH]} {
  set CIM_BASE_A_WIDTH 4
}

if {![info exists CIM_BASE_B_WIDTH]} {
  set CIM_BASE_B_WIDTH 4
}

if {![info exists CIM_BASE_C_WIDTH]} {
  set CIM_BASE_C_WIDTH 20
}

if {![info exists CIM_WRITE_CH_IN]} {
  set CIM_WRITE_CH_IN 1
}

if {![info exists CIM_MAC_LATENCY]} {
  set CIM_MAC_LATENCY 1
}

if {![info exists CIM_MODE]} {
  set CIM_MODE 0
}

if {![info exists CIM_SIGNED]} {
  set CIM_SIGNED true
}

if {![info exists CIM_TILE_INPUT_AXIS_ELEMENTS]} {
  set CIM_TILE_INPUT_AXIS_ELEMENTS 1
}

if {![info exists CIM_TILE_OUTPUT_AXIS_ELEMENTS]} {
  set CIM_TILE_OUTPUT_AXIS_ELEMENTS 4
}

if {![info exists CIM_INPUT_AXIS_TILES]} {
  set CIM_INPUT_AXIS_TILES 1
}

if {![info exists CIM_OUTPUT_AXIS_TILES]} {
  set CIM_OUTPUT_AXIS_TILES 1
}

if {![info exists CIM_A_PORT_TILES]} {
  set CIM_A_PORT_TILES $CIM_INPUT_AXIS_TILES
}

if {![info exists CIM_B_PORT_TILES]} {
  set CIM_B_PORT_TILES $CIM_OUTPUT_AXIS_TILES
}

if {![info exists CIM_C_BEAT_LAYOUT]} {
  set CIM_C_BEAT_LAYOUT [expr {$MATRIX_BACKEND == $MATRIX_BACKEND_CIM ? 1 : 0}]
}

if {$CIM_C_BEAT_LAYOUT != 0 && $CIM_C_BEAT_LAYOUT != 1} {
  error "CIM_C_BEAT_LAYOUT must be 0 (input-major) or 1 (output-major)"
}

if {![info exists CIM_C_PORT_TILES]} {
  if {$CIM_C_BEAT_LAYOUT == 0} {
    set CIM_C_PORT_TILES $CIM_INPUT_AXIS_TILES
  } else {
    set CIM_C_PORT_TILES $CIM_OUTPUT_AXIS_TILES
  }
}

if {$CIM_TILE_INPUT_AXIS_ELEMENTS <= 0 || $CIM_TILE_OUTPUT_AXIS_ELEMENTS <= 0} {
  error "CIM_TILE_INPUT_AXIS_ELEMENTS and CIM_TILE_OUTPUT_AXIS_ELEMENTS must be positive"
}
if {$CIM_INPUT_AXIS_TILES <= 0 || $CIM_OUTPUT_AXIS_TILES <= 0} {
  error "CIM input and output axis tile counts must be positive"
}
if {$CIM_A_PORT_TILES != $CIM_INPUT_AXIS_TILES} {
  error "CIM_A_PORT_TILES must currently span CIM_INPUT_AXIS_TILES"
}
if {$CIM_B_PORT_TILES <= 0 || $CIM_B_PORT_TILES > $CIM_OUTPUT_AXIS_TILES} {
  error "CIM_B_PORT_TILES must be positive and no wider than CIM_OUTPUT_AXIS_TILES"
}
if {$CIM_OUTPUT_AXIS_TILES % $CIM_B_PORT_TILES != 0} {
  error "CIM_B_PORT_TILES must evenly divide CIM_OUTPUT_AXIS_TILES"
}
if {$CIM_C_PORT_TILES <= 0} {
  error "CIM_C_PORT_TILES must be positive"
}

if {$MATRIX_BACKEND == $MATRIX_BACKEND_CIM} {
  if {$DATATYPE != "INT8" && $DATATYPE != "INT8_32"} {
    error "CIMProcessor currently supports INT8 and INT8_32 only"
  }
  if {$SUPPORT_MX} {
    error "CIMProcessor currently does not support microscaling"
  }
  if {$CIM_WRITE_CH_IN != 1} {
    error "CIMProcessor currently requires CIM_WRITE_CH_IN=1"
  }
  if {$CIM_B_SETS < 2} {
    error "CIMProcessor currently requires at least two resident B sets"
  }
  if {$CIM_C_BEAT_LAYOUT != 1} {
    error "CIMProcessor currently requires output-major C ports"
  }
  if {$CIM_C_PORT_TILES != $CIM_OUTPUT_AXIS_TILES} {
    error "CIMProcessor currently requires CIM_C_PORT_TILES to span the output axis"
  }

  set cim_input_scalars [expr {$CIM_CH_IN * $CIM_TILE_INPUT_AXIS_ELEMENTS * $CIM_INPUT_AXIS_TILES}]
  if {$cim_input_scalars != $IC_DIMENSION} {
    error "CIMProcessor input extent $cim_input_scalars must equal IC_DIMENSION $IC_DIMENSION"
  }
  if {$IC_DIMENSION ni {4 8 16 32 64}} {
    error "CIMProcessor currently requires IC_DIMENSION in {4 8 16 32 64}"
  }

  set cim_b_slices [expr {$WEIGHT_DTYPE_WIDTH / $CIM_BASE_B_WIDTH}]
  set cim_element_ch_out [expr {$CIM_CH_OUT / $cim_b_slices}]
  set cim_tile_ch_out [expr {$cim_element_ch_out * $CIM_TILE_OUTPUT_AXIS_ELEMENTS}]
  set cim_output_scalars [expr {$cim_tile_ch_out * $CIM_OUTPUT_AXIS_TILES}]
  if {$cim_output_scalars != $OC_DIMENSION} {
    error "CIMProcessor output extent $cim_output_scalars must equal OC_DIMENSION $OC_DIMENSION"
  }
}

# ================================================================
# Port Width Definitions
# ================================================================

if {![info exists IC_PORT_WIDTH]} {
  set IC_PORT_WIDTH [expr {$IC_DIMENSION * $INPUT_DTYPE_WIDTH}]
}

if {![info exists OC_PORT_WIDTH]} {
  set OC_PORT_WIDTH [expr {$OC_DIMENSION * $WEIGHT_DTYPE_WIDTH}]
}

# ================================================================
# Buffer Configurations
# ================================================================

if {![info exists INPUT_BUFFER_SIZE]} {
  set INPUT_BUFFER_SIZE 1024
}

if {![info exists INPUT_BUFFER_WIDTH]} {
  set INPUT_BUFFER_WIDTH [expr {$IC_DIMENSION * $INPUT_DTYPE_WIDTH}]
}

if {![info exists WEIGHT_BUFFER_SIZE]} {
  set WEIGHT_BUFFER_SIZE 1024
}

if {![info exists WEIGHT_BUFFER_WIDTH]} {
  set WEIGHT_BUFFER_WIDTH [expr {$OC_DIMENSION * $WEIGHT_DTYPE_WIDTH}]
}

# One processor weight-channel transfer
# Systolic transfers one buffer row; CIM transfers one physical B-port beat
if {$MATRIX_BACKEND == $MATRIX_BACKEND_CIM} {
  set WEIGHT_WRITE_WIDTH [expr {$CIM_WRITE_CH_IN * $CIM_B_PORT_TILES \
      * $CIM_TILE_OUTPUT_AXIS_ELEMENTS * $CIM_CH_OUT * $CIM_BASE_B_WIDTH}]
} else {
  set WEIGHT_WRITE_WIDTH $WEIGHT_BUFFER_WIDTH
}

if {![info exists ACCUM_BUFFER_SIZE]} {
  set ACCUM_BUFFER_SIZE 1024
}

# ================================================================
# DwC Configurations
# ================================================================

if {$SUPPORT_DWC} {
  set DWC_WIDTH 40
  set UNROLLFACTOR $OC_DIMENSION
  set DWC_KERNEL_DIM 3
  set DWC_KERNEL_SIZE [expr $DWC_KERNEL_DIM * $DWC_KERNEL_DIM]
  set DWC_DATATYPE $INPUT_DATATYPE
  set DWC_PSUM $ACCUM_DATATYPE
}
