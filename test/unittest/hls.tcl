# Standalone Catapult HLS driver for unittest blocks
#
# Unlike scripts/main.tcl (which compiles the whole Accelerator design and then
# picks a sub-block as top), this driver synthesizes one block in isolation: it
# feeds only the requested block header in as the design source. Child modules
# reach HLS through the selected top, matching how the full design sees them
#
# All inputs arrive through environment variables set by test/unittest/Makefile

proc env_required {name} {
  if {![info exists ::env($name)] || $::env($name) eq ""} {
    puts stderr "hls.tcl: required environment variable $name is not set"
    exit 2
  }
  return $::env($name)
}

set BLOCK          [env_required BLOCK]
set TOP            [env_required HLS_TOP]
set ROOT           [file normalize [env_required REPO_ROOT]]
set DESIGN_HEADER  [env_required DESIGN_HEADER]
set PROJECT_DIR    [env_required HLS_PROJECT_DIR]
set CLOCK_PERIOD   [env_required CLOCK_PERIOD]
set TECHNOLOGY     [env_required TECHNOLOGY]
set CLOCK_PORTS    [env_required HLS_CLOCKS]
set RESET_NAME     [env_required HLS_RESET_NAME]

# Datatype defines only need to be valid enough for ArchitectureParams.h to
# resolve the operand widths; the top's widths are pinned explicitly in $TOP.
set DATATYPE       [env_required DATATYPE]
set IC_DIMENSION   [env_required IC_DIMENSION]
set OC_DIMENSION   [env_required OC_DIMENSION]

# Optional VCS SCVerify cosim. Attach the requested self-checking testbench and
# enable Catapult's SCVerify+VCS flow so the block can be co-simulated (C++
# model and extracted RTL) against the golden; set HLS_SCVERIFY=1
set SCVERIFY 0
if {[info exists ::env(HLS_SCVERIFY)] && $::env(HLS_SCVERIFY) eq "1"} {
  set SCVERIFY 1
}
set SCVERIFY_TB ""
if {$SCVERIFY} {
  set SCVERIFY_TB [env_required HLS_SCVERIFY_TB]
}

# --- Fresh project -----------------------------------------------------------
if {[file exists $PROJECT_DIR]} {
  file delete -force -- $PROJECT_DIR
}
project new -dir $PROJECT_DIR
project save

solution options set Project/SolutionName $BLOCK
solution options set Input/TargetPlatform x86_64
solution options set Input/CppStandard c++17
# Expose the source/lib roots to the C++ compiler for the block header and its
# dependencies. Note: these go through Input/CompilerFlags (-I), NOT
# Input/SearchPath. Input/SearchPath is also used to locate ac_blackbox()
# verilog_files, and putting $ROOT/src there makes Catapult *inline* the CIM
# "CIM/*.sv" blackbox RTL into the simulation netlist -- forcing a SystemVerilog
# concat that VCS's SystemC cosim mis-handles. Keeping src off the Verilog search
# path leaves the blackbox external and the sim concat Verilog (matching the
# global scripts/utils/setup_project.tcl flow).
set compiler_flags "-D$DATATYPE -DIC_DIMENSION=$IC_DIMENSION -DOC_DIMENSION=$OC_DIMENSION -I$ROOT/src -I$ROOT/lib -I$ROOT"
if {[info exists ::env(CIM_C_PORT_ORIENTATION)]} {
  append compiler_flags " -DCIM_TEST_C_PORT_ORIENTATION=$::env(CIM_C_PORT_ORIENTATION)"
}
solution options set Input/CompilerFlags $compiler_flags

solution options set Input/SearchPath "$ROOT/lib" -append

solution options set Output/OutputVHDL false
solution options set Output/MaxNameLength 256
solution options set Output/MaxNameLengthModule 256
solution options set Output/MaxNameLengthInstance 256
solution options set Output/MaxNameLengthPort 256
solution options set Output/MaxNameLengthRegWire 256

# --- SCVerify (optional, VCS) -----------------------------------------------
# Mirror scripts/utils/setup_project.tcl's SCVerify+VCS setup so the standalone
# block can be co-simulated against the shared golden testbench under VCS. The
# testbench (added below, -exclude) drives the DUT via CCS_DESIGN and self-checks
if {$SCVERIFY} {
  solution options set Flows/Enable-SCVerify yes
  solution options set Flows/VCS/SYSC_VERSION 2.3.3
  # +incdir for the blackboxed CIM macro RTL: concat_sim_rtl.sv `include`s
  # cim_typedefs.svh from the blackbox RTL under src/CIM.
  solution options set Flows/VCS/VLOGAN_OPTS \
    "+v2k -timescale=1ns/10ps +notimingcheck +define+UNIT_DELAY +incdir+$ROOT/src/CIM"
  solution options set Flows/VCS/VCSSIM_OPTS {+vcs+lic+wait}
  solution options set Flows/VCS/COMP_FLAGS \
    "-O3 -Wall -Wno-unknown-pragmas -Wno-deprecated-declarations -I$ROOT/src -I$ROOT/lib -I$ROOT -I$ROOT/test/unittest -D$DATATYPE -DIC_DIMENSION=$IC_DIMENSION -DOC_DIMENSION=$OC_DIMENSION -DCIM_TEST_C_PORT_ORIENTATION=$::env(CIM_C_PORT_ORIENTATION) -DSCVERIFY -std=c++17"
  solution options set Flows/VCS/VCSELAB_OPTS \
    "-timescale=1ns/1ps -sysc=blocksync -lstdc++fs -lpthread"
  flow package require /SCVerify
  flow package option set /SCVerify/USE_VCS true
}

# --- Analyze / set top / compile --------------------------------------------
go new

# The standalone flow analyzes only the block header, so no enclosing module
# instantiates the requested template specialization. Generate a tiny translation
# unit that makes HLS_TOP visible; otherwise Catapult only sees helper ccores
# from included headers and `solution design set $TOP -top` is ignored.
set top_source [file join $PROJECT_DIR "hls_top_inst.cc"]
set top_fh [open $top_source w]
puts $top_fh [format {#include "%s"} $DESIGN_HEADER]
puts $top_fh [format {template class %s;} $TOP]
close $top_fh
solution file add $top_source -type C++

# Attach the SystemC testbench in SCVerify mode (excluded from synthesis)
# Its pinned SCVerify geometry must match HLS_TOP for the requested block
if {$SCVERIFY} {
  solution file add $SCVERIFY_TB -type C++ -exclude true
}
go analyze

solution design set $TOP -top
go compile

# --- Technology library ------------------------------------------------------
# Source the tech file directly by absolute path (setup_tech.tcl uses paths
# relative to the repo root, which this driver does not assume as CWD).
set tech_file "$ROOT/scripts/tech/$TECHNOLOGY.tcl"
if {![file exists $tech_file]} {
  puts stderr "hls.tcl: technology '$TECHNOLOGY' not found ($tech_file)"
  exit 1
}
source $tech_file
go libraries

# --- Clocks / reset ----------------------------------------------------------
# Build a -CLOCKS argument for the requested clock ports; reset is active-low rstn
# used by all CIM blocks (async_reset_signal_is(rstn, false)).
set clocks_arg ""
foreach clock_port $CLOCK_PORTS {
  append clocks_arg "$clock_port \"-CLOCK_PERIOD $CLOCK_PERIOD -CLOCK_EDGE rising -CLOCK_HIGH_TIME [expr $CLOCK_PERIOD / 2.0] -CLOCK_OFFSET 0.000000 -CLOCK_UNCERTAINTY 0.0 -RESET_KIND async -RESET_ASYNC_NAME $RESET_NAME -RESET_ASYNC_ACTIVE low -ENABLE_NAME {} -ENABLE_ACTIVE high\" "
}
directive set -CLOCKS $clocks_arg
directive set -CLOCK_OVERHEAD 0

# --- Schedule / extract RTL --------------------------------------------------
go assembly

# Match the production CIMArray resource mapping for the wide A receive beat
if {$BLOCK eq "CIMArray"} {
  set top_stripped [string map {" " ""} $TOP]
  directive set /$top_stripped/issue_mac/while:a_beat.value.value:rsc -MAP_TO_MODULE {[Register]}
}

go architect
go extract
project save

puts "hls.tcl: finished HLS for $BLOCK ($TOP)"
