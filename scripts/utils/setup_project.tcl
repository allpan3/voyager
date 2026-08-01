# Create project folder
set project_folder "$ROOT/$CATAPULT_BUILD_DIR/$BLOCK"
if { [file exists $project_folder] } {
  file delete -force -- $project_folder
}

project new -dir $project_folder
project save

# Configure options
solution options set Project/SolutionName $BLOCK
solution options set Message/ErrorOverride ASSERT-1 -remove
solution options set Input/TargetPlatform x86_64
solution options set Input/CppStandard c++17
set cim_compiler_flags "-DMATRIX_BACKEND=$MATRIX_BACKEND -DCIM_CH_IN=$CIM_CH_IN -DCIM_CH_OUT=$CIM_CH_OUT -DCIM_B_SETS=$CIM_B_SETS -DCIM_BASE_A_WIDTH=$CIM_BASE_A_WIDTH -DCIM_BASE_B_WIDTH=$CIM_BASE_B_WIDTH -DCIM_BASE_C_WIDTH=$CIM_BASE_C_WIDTH -DCIM_WRITE_CH_IN=$CIM_WRITE_CH_IN -DCIM_MAC_LATENCY=$CIM_MAC_LATENCY -DCIM_MODE=$CIM_MODE -DCIM_TILE_INPUT_AXIS_ELEMENTS=$CIM_TILE_INPUT_AXIS_ELEMENTS -DCIM_TILE_OUTPUT_AXIS_ELEMENTS=$CIM_TILE_OUTPUT_AXIS_ELEMENTS -DCIM_INPUT_AXIS_TILES=$CIM_INPUT_AXIS_TILES -DCIM_OUTPUT_AXIS_TILES=$CIM_OUTPUT_AXIS_TILES -DCIM_A_PORT_TILES=$CIM_A_PORT_TILES -DCIM_B_PORT_TILES=$CIM_B_PORT_TILES -DCIM_C_PORT_TILES=$CIM_C_PORT_TILES -DCIM_C_BEAT_LAYOUT=$CIM_C_BEAT_LAYOUT -DCIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE=$CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE"
solution options set Input/CompilerFlags "-D$DATATYPE -DIC_DIMENSION=$IC_DIMENSION -DOC_DIMENSION=$OC_DIMENSION -DINPUT_BUFFER_SIZE=$INPUT_BUFFER_SIZE -DWEIGHT_BUFFER_SIZE=$WEIGHT_BUFFER_SIZE -DACCUM_BUFFER_SIZE=$ACCUM_BUFFER_SIZE -DDOUBLE_BUFFERED_ACCUM_BUFFER=$DOUBLE_BUFFERED_ACCUM_BUFFER -DSUPPORT_MVM=$SUPPORT_MVM -DSUPPORT_SPMM=$SUPPORT_SPMM -DSUPPORT_DWC=$SUPPORT_DWC -DCLOCK_PERIOD=$CLOCK_PERIOD -DENABLE_PERF_COUNTERS=$ENABLE_PERF_COUNTERS $cim_compiler_flags"
solution options set Input/SearchPath "$ROOT/lib"
solution options set Architectural/DefaultMemMapThreshold 1024
solution options set Architectural/DefaultRegisterThreshold 4096
solution options set Flows/Enable-SCVerify yes
solution options set Flows/VCS/SYSC_VERSION 2.3.3
solution options set Flows/VCS/VLOGAN_OPTS {+v2k -timescale=1ns/10ps +notimingcheck +define+UNIT_DELAY}
# solution options set Flows/VCS/VCSSIM_OPTS {+fsdbfile+dump.fsdb +fsdb+all=on +fsdb+dumpon+0}
solution options set Flows/VCS/VCSSIM_OPTS {+vcs+lic+wait}
solution options set Flows/VCS/VCS_DOFILE "$ROOT/utils/dump.do"
solution options set Flows/VCS/COMP_FLAGS "-O3 -Wall -Wno-unknown-pragmas -I$ROOT/lib/ -I$ROOT/lib/xtensor/include -I$ROOT/lib/xtl/include -I$ROOT/lib/spdlog/include -I$ROOT/src/ -I$ROOT/ -I$::env(CONDA_PREFIX)/include -DSIM_$BLOCK -D$DATATYPE -DIC_DIMENSION=$IC_DIMENSION -DOC_DIMENSION=$OC_DIMENSION -DINPUT_BUFFER_SIZE=$INPUT_BUFFER_SIZE -DWEIGHT_BUFFER_SIZE=$WEIGHT_BUFFER_SIZE -DACCUM_BUFFER_SIZE=$ACCUM_BUFFER_SIZE -DDOUBLE_BUFFERED_ACCUM_BUFFER=$DOUBLE_BUFFERED_ACCUM_BUFFER -DSUPPORT_MVM=$SUPPORT_MVM -DSUPPORT_DWC=$SUPPORT_DWC -DENABLE_PERF_COUNTERS=$ENABLE_PERF_COUNTERS -std=c++17 $cim_compiler_flags"
solution options set Flows/VCS/VCSELAB_OPTS "-timescale=1ns/1ps -sysc=blocksync -lstdc++fs -L$::env(CONDA_PREFIX)/lib -LDFLAGS \"-Wl,--enable-new-dtags -Wl,-R,$::env(CONDA_PREFIX)/lib\" -labsl_hash -labsl_log_internal_check_op -labsl_log_internal_message -labsl_log_internal_nullguard -lprotobuf -lpthread"
solution options set Cache/UserCacheHome "$ROOT/$CATAPULT_BUILD_DIR/cache"
solution options set Cache/DefaultCacheHomeEnabled false

solution options set Output/OutputVHDL false
solution options set Output/MaxNameLength 256
solution options set Output/MaxNameLengthModule 256
solution options set Output/MaxNameLengthInstance 256
solution options set Output/MaxNameLengthPort 256
solution options set Output/MaxNameLengthRegWire 256

flow package require /SCVerify
flow package option set /SCVerify/USE_VCS true

go new

# Add source files
solution file add $ROOT/src/Accelerator.h -type CHEADER

# Add testbench files
solution file add $ROOT/test/common/TestRunner.cc -type C++ -exclude true
solution file add $ROOT/test/common/Harness.cc -type C++ -exclude true
solution file add $ROOT/test/common/GoldModel.cc -type C++ -exclude true
solution file add $ROOT/test/common/Simulation.cc -type C++ -exclude true
solution file add $ROOT/test/common/ArrayMemory.cc -type C++ -exclude true
solution file add $ROOT/test/common/DataLoader.cc -type C++ -exclude true
solution file add $ROOT/test/common/AccessCounter.cc -type C++ -exclude true
solution file add $ROOT/test/toolchain/MapOperation.cc -type C++ -exclude true

# Add network files
solution file add $ROOT/test/common/Network.cc -type C++ -exclude true
solution file add $ROOT/test/common/Tiling.cc -type C++ -exclude true
solution file add $ROOT/test/compiler/proto/param.pb.cc -type C++ -exclude true
solution file add $ROOT/test/compiler/proto/tiling.pb.cc -type C++ -exclude true
