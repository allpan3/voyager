# Catapult block definition for the CIMArray lane wrapper

set block "CIMArray"
set full_block_name "CIMArray<$CIM_CH_IN, $CIM_CH_OUT, $CIM_B_SETS, $CIM_BASE_A_WIDTH, $CIM_BASE_B_WIDTH, $CIM_BASE_C_WIDTH, $CIM_WRITE_CH_IN, $CIM_MAC_LATENCY, $CIM_MODE, $INPUT_DTYPE_WIDTH, $WEIGHT_DTYPE_WIDTH, $CIM_SIGNED, $CIM_TILE_INPUT_LANES, $CIM_TILE_OUTPUT_LANES, $CIM_REDUCTION_GROUPS, $CIM_MULTICAST_GROUPS, $CIM_A_PORT_TILES, $CIM_B_PORT_TILES, $CIM_C_PORT_TILES, $CIM_C_PORT_ORIENTATION>"

# Keep the wide A receive beat in registers so issue_mac can sustain II=1
proc pre_architect {} {
  global full_block_name
  set cim_array_stripped [string map {" " ""} $full_block_name]

  directive set /$cim_array_stripped/issue_mac/while:a_beat.value.value:rsc -MAP_TO_MODULE {[Register]}
}
