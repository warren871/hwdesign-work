open_project fir_proj
set_top fir_accel
add_files fir_top.cpp
add_files -tb fir_tb.cpp
open_solution "solution1"
set_part {xc7z020clg484-1}
create_clock -period 10 -name default
csim_design
csynth_design
cosim_design -rtl verilog -trace_level port
exit
