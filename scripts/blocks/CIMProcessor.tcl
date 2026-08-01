# Catapult block definition for the strict integer CIM matrix backend

set block "CIMProcessor"
set full_block_name "CIMProcessor<InputTypeList, WeightTypeList, $SA_INPUT_TYPE, $SA_WEIGHT_TYPE, $ACCUM_DATATYPE, $ACCUM_BUFFER_DATATYPE, $SCALE_DATATYPE, $IC_DIMENSION, $OC_DIMENSION, $ACCUM_BUFFER_SIZE, $CIM_CH_IN, $CIM_CH_OUT, $CIM_B_SETS, $CIM_BASE_A_WIDTH, $CIM_BASE_B_WIDTH, $CIM_BASE_C_WIDTH, $CIM_WRITE_CH_IN, $CIM_MAC_LATENCY, $CIM_MODE, $CIM_SIGNED, $CIM_TILE_INPUT_AXIS_ELEMENTS, $CIM_TILE_OUTPUT_AXIS_ELEMENTS, $CIM_INPUT_AXIS_TILES, $CIM_OUTPUT_AXIS_TILES, $CIM_A_PORT_TILES, $CIM_B_PORT_TILES, $CIM_C_PORT_TILES, $CIM_C_BEAT_LAYOUT, $CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE>"
set full_block_name_stripped [string map {" " ""} $full_block_name]
set cim_array_name "CIMArray<$CIM_CH_IN, $CIM_CH_OUT, $CIM_B_SETS, $CIM_BASE_A_WIDTH, $CIM_BASE_B_WIDTH, $CIM_BASE_C_WIDTH, $CIM_WRITE_CH_IN, $CIM_MAC_LATENCY, $CIM_MODE, $INPUT_DTYPE_WIDTH, $WEIGHT_DTYPE_WIDTH, $ACCUM_DATATYPE_WIDTH, $CIM_SIGNED, $CIM_TILE_INPUT_AXIS_ELEMENTS, $CIM_TILE_OUTPUT_AXIS_ELEMENTS, $CIM_INPUT_AXIS_TILES, $CIM_OUTPUT_AXIS_TILES, $CIM_A_PORT_TILES, $CIM_B_PORT_TILES, $CIM_C_PORT_TILES, $CIM_C_BEAT_LAYOUT, $CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE>"
set cim_array_name_stripped [string map {" " ""} $cim_array_name]

# Compile the nested array as the separately verified CIMArray block
proc pre_compile {} {
  global cim_array_name
  solution design set $cim_array_name -mapped
}

# Add the separately synthesized CIMArray implementation
proc pre_libraries {} {
  solution library add {[Block] CIMArray.v1}
}

# Bind the nested array instance to the verified CIMArray implementation
proc pre_assembly {} {
  global full_block_name_stripped cim_array_name_stripped
  directive set /$full_block_name_stripped/$cim_array_name_stripped -MAP_TO_MODULE {[Block] CIMArray.v1}
}

# Accumulator requests and responses are decoupled across independent threads
