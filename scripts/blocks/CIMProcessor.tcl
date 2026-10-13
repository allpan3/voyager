set block CIMProcessor
set full_block_name [cim_processor_template]
set full_block_name_stripped [string map {" " ""} $full_block_name]
set cim_array_name [cim_array_template]
set cim_array_name_stripped [string map {" " ""} $cim_array_name]

proc pre_compile {} {
  global cim_array_name
  solution design set $cim_array_name -mapped
}

proc pre_libraries {} {
  solution library add {[Block] CIMArray.v1}
}

proc pre_assembly {} {
  global full_block_name_stripped cim_array_name_stripped
  directive set /$full_block_name_stripped/$cim_array_name_stripped \
    -MAP_TO_MODULE {[Block] CIMArray.v1}
}

proc pre_architect {} {
  global full_block_name_stripped
  set local_context_path "/$full_block_name_stripped/$full_block_name_stripped:complete_accumulation/complete_accumulation/local_accum_context_values.value.int_val:rsc"
  directive set $local_context_path -MAP_TO_MODULE {[Register]}
}

proc pre_extract {} {
  global DOUBLE_BUFFERED_ACCUM_BUFFER
  # The writer releases each bank after its final store.
  if {$DOUBLE_BUFFERED_ACCUM_BUFFER == true} {
    cycle set accumulation_buffer_done.sync_out() -from accumulation_buffer_write_request.Push() -equal 1
    cycle set accumulation_buffer_done.sync_out()#1 -from accumulation_buffer_write_request.Push()#1 -equal 1
    cycle set accum_to_wb_enq.Push() -from accumulation_buffer_write_request.Push() -min 1
    cycle set accum_to_wb_enq.Push() -from accumulation_buffer_write_request.Push()#1 -min 1
  }
}
