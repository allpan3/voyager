# Catapult block definition for the strict integer CIM matrix backend

set block "CIMProcessor"
set full_block_name "CIMProcessor<InputTypeList, WeightTypeList, $SA_INPUT_TYPE, $SA_WEIGHT_TYPE, $ACCUM_DATATYPE, $ACCUM_BUFFER_DATATYPE, $SCALE_DATATYPE, $IC_DIMENSION, $OC_DIMENSION, $ACCUM_BUFFER_SIZE, $CIM_CH_IN, $CIM_CH_OUT, $CIM_B_SETS, $CIM_BASE_A_WIDTH, $CIM_BASE_B_WIDTH, $CIM_BASE_C_WIDTH, $CIM_WRITE_CH_IN, $CIM_MAC_LATENCY, $CIM_MODE, $INPUT_DTYPE_WIDTH, $WEIGHT_DTYPE_WIDTH, $CIM_SIGNED, $CIM_TILE_INPUT_LANES, $CIM_TILE_OUTPUT_LANES, $CIM_REDUCTION_GROUPS, $CIM_MULTICAST_GROUPS, $CIM_A_PORT_TILES, $CIM_B_PORT_TILES, $CIM_C_PORT_TILES, $CIM_C_PORT_ORIENTATION>"
set full_block_name_stripped [string map {" " ""} $full_block_name]
set cim_array_name "CIMArray<$CIM_CH_IN, $CIM_CH_OUT, $CIM_B_SETS, $CIM_BASE_A_WIDTH, $CIM_BASE_B_WIDTH, $CIM_BASE_C_WIDTH, $CIM_WRITE_CH_IN, $CIM_MAC_LATENCY, $CIM_MODE, $INPUT_DTYPE_WIDTH, $WEIGHT_DTYPE_WIDTH, $CIM_SIGNED, $CIM_TILE_INPUT_LANES, $CIM_TILE_OUTPUT_LANES, $CIM_REDUCTION_GROUPS, $CIM_MULTICAST_GROUPS, $CIM_A_PORT_TILES, $CIM_B_PORT_TILES, $CIM_C_PORT_TILES, $CIM_C_PORT_ORIENTATION>"
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

# Preserve the MatrixUnit accumulation-buffer response latency contract
proc pre_extract {} {
  global DOUBLE_BUFFERED_ACCUM_BUFFER
  cycle set accumulation_buffer_read_data.Pop() -from accumulation_buffer_read_address.Push() -equal 2
  if {$DOUBLE_BUFFERED_ACCUM_BUFFER == true} {
    cycle set accumulation_buffer_read_data.Pop()#1 -from accumulation_buffer_read_address.Push()#1 -equal 2
  }
}
