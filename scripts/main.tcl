# Setup environment variables and design parameters
source scripts/utils/read_env_vars.tcl
source scripts/architecture.tcl

# Dummy procedures that can be overriden by block-specific scripts
proc pre_analyze {} {}
proc pre_compile {} {}
proc pre_libraries {} {}
proc pre_assembly {} {}
proc pre_architect {} {}
proc pre_extract {} {}

# Load block-specific script
source scripts/blocks/$BLOCK.tcl

# Setup project setting and folder
source scripts/utils/setup_project.tcl

# Record the target configuration and resolved HLS inputs before synthesis
set mapping_artifact "$project_folder/$BLOCK.v1/concat_rtl.v"
set mapping_command [list exec env "PYTHONPATH=$ROOT/voyager-compiler/src:$ROOT/interstellar/src:$ROOT/test/compiler" python "$ROOT/test/compiler/mapping_target.py"]
set mapping_settings [list --setting "IC_PORT_WIDTH=$IC_PORT_WIDTH" --setting "OC_PORT_WIDTH=$OC_PORT_WIDTH" --setting "CLOCK_PERIOD=$CLOCK_PERIOD" --setting "TECHNOLOGY=$TECHNOLOGY" --setting "CompilerFlags=$mapping_compiler_flags"]
{*}$mapping_command begin --target $::env(MAPPING_TARGET) --artifact $mapping_artifact {*}$mapping_settings 2>@1

pre_analyze
go analyze

solution design set $full_block_name -top

pre_compile
go compile

# set technology library
source scripts/tech/setup_tech.tcl

# Add build folder
solution options set ComponentLibs/SearchPath [file dirname $project_folder] -append

pre_libraries
go libraries

if { [info exists clock_multiplier] } {
  set CLOCK_PERIOD [expr $CLOCK_PERIOD * $clock_multiplier]
}

directive set -CLOCKS "clk \"-CLOCK_PERIOD $CLOCK_PERIOD -CLOCK_EDGE rising -CLOCK_HIGH_TIME [expr $CLOCK_PERIOD / 2.0] -CLOCK_OFFSET 0.000000 -CLOCK_UNCERTAINTY 0.0 -RESET_KIND async -RESET_SYNC_NAME rst -RESET_SYNC_ACTIVE high -RESET_ASYNC_NAME arst_n -RESET_ASYNC_ACTIVE low -ENABLE_NAME {} -ENABLE_ACTIVE high\" "
directive set -CLOCK_OVERHEAD 0

pre_assembly
go assembly

pre_architect
go architect

pre_extract
go extract

# Publish the build settings after synthesis produces the RTL artifact
if {![file isfile $mapping_artifact] && [file isfile "[file rootname $mapping_artifact].sv"]} {
  set mapping_artifact "[file rootname $mapping_artifact].sv"
}
{*}$mapping_command finish --target $::env(MAPPING_TARGET) --artifact $mapping_artifact 2>@1
