set block "MatrixUnit"
set full_block_name "MatrixUnit"

# Separately generated MatrixUnit child blocks
set matrix_unit_blocks [list \
  InputController "InputController<InputTypeList, $IC_DIMENSION, $IC_PORT_WIDTH, $INPUT_BUFFER_WIDTH>" \
  WeightController "WeightController<WeightTypeList, $ACCUM_BUFFER_DATATYPE, $IC_DIMENSION, $OC_DIMENSION, $OC_PORT_WIDTH, $WEIGHT_BUFFER_WIDTH, $WEIGHT_WRITE_WIDTH, $CIM_B_SETS>" \
]
if {$MATRIX_BACKEND == $MATRIX_BACKEND_CIM} {
  lappend matrix_unit_blocks CIMProcessor \
    "CIMProcessor<InputTypeList, WeightTypeList, $SA_INPUT_TYPE, $SA_WEIGHT_TYPE, $ACCUM_DATATYPE, $ACCUM_BUFFER_DATATYPE, $SCALE_DATATYPE, $IC_DIMENSION, $OC_DIMENSION, $ACCUM_BUFFER_SIZE, $CIM_CH_IN, $CIM_CH_OUT, $CIM_B_SETS, $CIM_BASE_A_WIDTH, $CIM_BASE_B_WIDTH, $CIM_BASE_C_WIDTH, $CIM_WRITE_CH_IN, $CIM_MAC_LATENCY, $CIM_MODE, $CIM_SIGNED, $CIM_TILE_INPUT_AXIS_ELEMENTS, $CIM_TILE_OUTPUT_AXIS_ELEMENTS, $CIM_INPUT_AXIS_TILES, $CIM_OUTPUT_AXIS_TILES, $CIM_A_PORT_TILES, $CIM_B_PORT_TILES, $CIM_C_PORT_TILES, $CIM_C_BEAT_LAYOUT, $CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE, $CIM_LOCAL_ACCUM_CONTEXTS>"
} else {
  lappend matrix_unit_blocks MatrixProcessor \
    "MatrixProcessor<InputTypeList, WeightTypeList, $SA_INPUT_TYPE, $SA_WEIGHT_TYPE, $ACCUM_DATATYPE, $ACCUM_BUFFER_DATATYPE, $SCALE_DATATYPE, $IC_DIMENSION, $OC_DIMENSION, $ACCUM_BUFFER_SIZE>"
}
lappend matrix_unit_blocks MatrixParamsDeserializer \
  "MatrixParamsDeserializer<0, [expr {$SUPPORT_MX ? 6 : 4}]>"

# Marks MatrixUnit child blocks for hierarchical Catapult assembly
proc pre_compile {} {
  global matrix_unit_blocks
  foreach {_ template} $matrix_unit_blocks {
    solution design set $template -mapped
  }
}

# Adds MatrixUnit child blocks to the component library
proc pre_libraries {} {
  global matrix_unit_blocks
  foreach {name _} $matrix_unit_blocks {
    solution library add [format {[Block] %s.v1} $name]
  }
}

# Maps MatrixUnit child instances to their generated Catapult blocks
proc pre_assembly {} {
  global matrix_unit_blocks
  foreach {name template} $matrix_unit_blocks {
    set instance [string map {" " ""} $template]
    directive set /MatrixUnit/$instance -MAP_TO_MODULE \
      [format {[Block] %s.v1} $name]
  }
}

# Configures one MatrixUnit double buffer
proc configure_double_buffer {template_name size width technology} {
  set stripped_name [string map {" " ""} $template_name]
  set base_path "/MatrixUnit/$stripped_name/$stripped_name"
  directive set ${base_path}:mem0_run/mem0_run/mem0 -WORD_WIDTH $width
  directive set ${base_path}:mem1_run/mem1_run/mem1 -WORD_WIDTH $width

  if {$technology != "generic" && $technology != "tsmc40" && $size > 32} {
    set memory_library [get_memory_name 1 $size $width]
    directive set ${base_path}:mem0_run/mem0_run/mem0:rsc -MAP_TO_MODULE $memory_library
    directive set ${base_path}:mem1_run/mem1_run/mem1:rsc -MAP_TO_MODULE $memory_library
  }
}

# Maps MatrixUnit-owned buffers
proc pre_architect {} {
  global TECHNOLOGY IC_DIMENSION OC_DIMENSION INPUT_BUFFER_SIZE \
         INPUT_BUFFER_WIDTH WEIGHT_BUFFER_SIZE WEIGHT_BUFFER_WIDTH \
         ACCUM_BUFFER_DATATYPE ACCUM_BUFFER_SIZE ACCUM_DATATYPE_WIDTH \
         ACC_BUF_C_DATA_REP_NAME SUPPORT_MX DOUBLE_BUFFERED_ACCUM_BUFFER \
         SCALE_DATATYPE_WIDTH MATRIX_BACKEND MATRIX_BACKEND_CIM

  configure_double_buffer \
    "DoubleBuffer<$INPUT_BUFFER_SIZE,$INPUT_BUFFER_WIDTH>" \
    $INPUT_BUFFER_SIZE $INPUT_BUFFER_WIDTH $TECHNOLOGY

  if {$MATRIX_BACKEND != $MATRIX_BACKEND_CIM} {
    configure_double_buffer \
      "DoubleBuffer<$WEIGHT_BUFFER_SIZE,$WEIGHT_BUFFER_WIDTH>" \
      $WEIGHT_BUFFER_SIZE $WEIGHT_BUFFER_WIDTH $TECHNOLOGY
  }

  if {$SUPPORT_MX == true} {
    configure_double_buffer \
      "DoubleBuffer<$INPUT_BUFFER_SIZE,$SCALE_DATATYPE_WIDTH>" \
      $INPUT_BUFFER_SIZE $SCALE_DATATYPE_WIDTH $TECHNOLOGY
    set weight_scale_size [expr {$WEIGHT_BUFFER_SIZE / $IC_DIMENSION}]
    set weight_scale_width [expr {$SCALE_DATATYPE_WIDTH * $OC_DIMENSION}]
    configure_double_buffer \
      "DoubleBuffer<$weight_scale_size,$weight_scale_width>" \
      $weight_scale_size $weight_scale_width $TECHNOLOGY
  }

  set accum_template \
    "DualPortBuffer<Pack1D<$ACCUM_BUFFER_DATATYPE,${OC_DIMENSION}UL>,$ACCUM_BUFFER_SIZE>"
  set accum_name [string map {" " ""} $accum_template]
  set accum_width [expr {$OC_DIMENSION * $ACCUM_DATATYPE_WIDTH}]
  set bank0_path "/MatrixUnit/$accum_name/bank0_run/bank0.value.$ACC_BUF_C_DATA_REP_NAME"
  set bank1_path "/MatrixUnit/$accum_name/bank1_run/bank1.value.$ACC_BUF_C_DATA_REP_NAME"
  directive set $bank0_path -WORD_WIDTH $accum_width
  if {$DOUBLE_BUFFERED_ACCUM_BUFFER == true} {
    directive set $bank1_path -WORD_WIDTH $accum_width
  }

  if {$TECHNOLOGY != "generic" && $TECHNOLOGY != "tsmc40"} {
    set memory_library [get_memory_name 0 $ACCUM_BUFFER_SIZE $accum_width]
    directive set ${bank0_path}:rsc -MAP_TO_MODULE $memory_library
    if {$DOUBLE_BUFFERED_ACCUM_BUFFER == true} {
      directive set ${bank1_path}:rsc -MAP_TO_MODULE $memory_library
    }
  }
}

# Removes false MatrixUnit memory ordering constraints before RTL extraction
proc pre_extract {} {
  global DOUBLE_BUFFERED_ACCUM_BUFFER
  ignore_memory_precedences -from WRITE_BANK_0* -to READ_BANK_0*
  if {$DOUBLE_BUFFERED_ACCUM_BUFFER == true} {
    ignore_memory_precedences -from WRITE_BANK_1* -to READ_BANK_1*
  }
}
