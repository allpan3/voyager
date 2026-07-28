# Catapult block definition for the CIMArray tile grid

set block "CIMArray"
set full_block_name "CIMArray<$CIM_CH_IN, $CIM_CH_OUT, $CIM_B_SETS, $CIM_BASE_A_WIDTH, $CIM_BASE_B_WIDTH, $CIM_BASE_C_WIDTH, $CIM_WRITE_CH_IN, $CIM_MAC_LATENCY, $CIM_MODE, $INPUT_DTYPE_WIDTH, $WEIGHT_DTYPE_WIDTH, $ACCUM_DATATYPE_WIDTH, $CIM_SIGNED, $CIM_TILE_INPUT_AXIS_ELEMENTS, $CIM_TILE_OUTPUT_AXIS_ELEMENTS, $CIM_INPUT_AXIS_TILES, $CIM_OUTPUT_AXIS_TILES, $CIM_A_PORT_TILES, $CIM_B_PORT_TILES, $CIM_C_PORT_TILES, $CIM_C_BEAT_LAYOUT>"
set full_block_name_stripped [string map {" " ""} $full_block_name]
set cim_tile_name "CIMTile<$CIM_CH_IN, $CIM_CH_OUT, $CIM_B_SETS, $CIM_BASE_A_WIDTH, $CIM_BASE_B_WIDTH, $CIM_BASE_C_WIDTH, $CIM_WRITE_CH_IN, $CIM_MAC_LATENCY, $CIM_MODE, $INPUT_DTYPE_WIDTH, $WEIGHT_DTYPE_WIDTH, $CIM_SIGNED, $CIM_TILE_INPUT_AXIS_ELEMENTS, $CIM_TILE_OUTPUT_AXIS_ELEMENTS>"
set cim_tile_name_stripped [string map {" " ""} $cim_tile_name]

# Compile each nested tile as a separately verified CIMTile block
proc pre_compile {} {
  global cim_tile_name
  solution design set $cim_tile_name -mapped
}

# Add the separately synthesized CIMTile implementation
proc pre_libraries {} {
  solution library add {[Block] CIMTile.v1}
}

# Bind every nested tile instance to the verified CIMTile implementation
proc pre_assembly {} {
  global full_block_name_stripped cim_tile_name_stripped
  directive set /$full_block_name_stripped/$cim_tile_name_stripped -MAP_TO_MODULE {[Block] CIMTile.v1}
}
