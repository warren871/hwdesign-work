# In-class accelerator: scale and offset

Build a Vitis HLS accelerator following the Waveflow streaming polynomial example, and follow the command-response pattern where the two differ; configuration is a command on the stream, not an AXI-Lite write. Write a bit-exact Python model, Vitis HLS kernel, and testbench. Run the Python model and C simulation, check thaat they agree bit for bit, and stop once C simulation passes; do not synthesize or run co-simulation.

## Operation and commands

Use IEEE 754 binary32 for coefficients, input and output. CONFIG supplies finite binary32 'a' and 'b', replaces both previous coefficients, and produces no response. DATA supplies 1–256 samples and returns one response header echoing 'tx_id' followed by exactly that many outputs. For each sample, calculate 'p = round_binary32(a*x)' and then 'y = round_binary32(p+b)'; do not fuse the operations. Accept finite samples and choose tests that do not overflow. A second CONFIG replaces both coefficients before the next DATA. END has no payload or response and causes normal return. Choose and document the packing and command codes consistently.

Each command header is its own TLAST-terminated burst; any payload is a separate burst with TLAST only on its last word. Validate the command and its parameters before reading payload. Buffer each DATA payload and validate its complete framing before emitting its response; thus a faulty DATA emits no response. For valid DATA, end the response header burst and the result data burst with TLAST. Reset all configuration and status each activation.

## Errors and checks

Use distinct status codes for unknown command, invalid length or parameter, DATA before CONFIG, early payload TLAST, and missing TLAST on the expected final payload word. On any error set 'halted', error code and offending 'tx_id'; close any opened output burst, return immediately without consuming further input, and require the host to reset its stream path before restarting. If a header has multiple errors, prioritize unknown command, then DATA-before-CONFIG, then invalid parameter. For an early TLAST stop as soon as observed; for a missing TLAST stop at the expected last word without draining. A command whose header framing is malformed must also halt with a separate header-framing code. State the precise non-finite input behavior chosen by the implementation and test it.

Independently check at least: 'a=2', 'b=1', 'x=[-2,0,3]' gives '[-3,1,7]'; a second CONFIG 'a=-1', 'b=0.5', 'x=[2,-1]' gives '[-1.5,1.5]'. Test multiple DATA commands, reconfiguration, lengths 1 and 256, END, and each error with a separate activation and a successful fresh activation afterwards. Check output transaction IDs and all TLASTs. Test that no response is emitted for an invalid DATA.

Write 'results.md' with a scenario table showing PASS/FAIL and evidence for each test, the actual output/status and recovery outcome for each error, and a list of every ambiguity in this prompt and the decision made. Do not claim tests were run if they were not.

