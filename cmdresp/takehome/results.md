# FIR accelerator results

The current implementation uses real AXI-Stream TLAST sidebands. The latest full hardware validation, including the added same-activation reconfiguration and precedence tests, is [fir_vitis_final.log](./fir_vitis_final.log); the latest independent Python run is [python_validation_tlast.log](./python_validation_tlast.log). The generated RTL interface is [fir_accel.v](./fir_proj/solution1/syn/verilog/fir_accel.v). The latest T=8/N=256 trace is [fir_t8_256_final.vcd](./vcd/fir_t8_256_final.vcd), with extraction tool [measure_fir_trace.py](./measure_fir_trace.py) and summary [fir_t8_256_final_measurement.txt](./fir_t8_256_final_measurement.txt). VCD-export output is [vcd_export_final.log](./vcd_export_final.log).

## Interface

- Command codes are `CONFIG = 0`, `DATA = 1`, and `END = 2`. Protocol words are 32 bits: command `[7:0]`, transaction ID `[23:8]`, command field `[31:24]`.
- Kernel and testbench use `ap_axis<32, 0, 0, 0>` on both top-level streams. Generated RTL has 32-bit `in_r_TDATA` and `out_r_TDATA` and distinct one-bit `in_r_TLAST` and `out_r_TLAST` ports, as shown in [fir_accel.v](./fir_proj/solution1/syn/verilog/fir_accel.v). `TKEEP` and `TSTRB` are set to all ones for each full 32-bit word.
- CONFIG is one TLAST-terminated input burst: command word (`T=1..8`, TLAST low), eight coefficient words (`h[0]..h[7]`, TLAST high only on `h[7]`). Only the first T coefficients are used.
- DATA has a two-word TLAST-terminated header burst: command word (TLAST low), then a full 32-bit count word (TLAST high). Counts are validated as full-width values; `1..256` are accepted, zero and values above 256 are rejected before the separate sample burst is read. The payload is N full 32-bit words with TLAST high only on the final sample; no packed-sample partial word is used.
- A valid response has a one-word response-header burst with echoed transaction ID and TLAST high, followed by a distinct N-word result burst with TLAST high only on the final result. Invalid DATA payloads produce no response.
- END is a one-word input burst with TLAST high and returns normally. CONFIG and DATA command words must have TLAST low; malformed command framing reports `ERR_MALFORMED_HEADER`.
- AXI-Lite status registers are `status_halted`, `status_error`, `status_txid`, and `status_cycles`. Error codes: malformed header 1, unknown command 2, invalid tap count 3, DATA before CONFIG 4, invalid sample count 5, early payload TLAST 6, missing final payload TLAST 7.
- After an error, the kernel stops without draining queued input. The host resets the stream and invokes a fresh activation; the testbench uses independent streams for each recovery.

## Design

- [fir_top.cpp](./fir_top.cpp) computes signed Q1.15 products in a wide accumulator, rounds ties to even, saturates to signed 16-bit, buffers and validates all N input samples before producing the response, and carries delay state between DATA commands until CONFIG resets it.
- The independent model [fir.py](./fir.py) represents each stream beat as `(32-bit word, TLAST)`; count validation uses the full word and does not mask high invalid bits. [fir_tb.cpp](./fir_tb.cpp) uses the same AXI stream type as the kernel, checks response transaction IDs and burst TLAST positions, and exercises separate fresh recovery activations after errors.
- In the T=8/N=256 VCD, 256 payload handshakes span 764 inclusive clock cycles, giving 0.3350785 samples/clock. The target of at least one sample/clock is missed; the measured RTL-side readiness stalls, not source bubbles, account for the difference. No timing restructuring was attempted.
- `status_cycles` is an accepted-sample count, not an elapsed-clock counter. The reported interval is independently calculated from rising edges of `ap_clk` where `in_r_TVALID && in_r_TREADY` are both high. Details and the sample-by-sample derivation are in [fir_t8_256_final_measurement.txt](./fir_t8_256_final_measurement.txt).
- The Vitis co-simulation transaction report gives transaction 7 (`fir_t8_256`) a latency of 14362 cycles. Synthesis estimates a 6.973 ns clock period. The payload-read loop is not pipelined and its achieved II is not reported (`-`); the measured transfer spacing is about three clock cycles per accepted sample.

## Targets

| Target | Measured value | Result | Evidence |
| --- | --- | --- | --- |
| Independent Python checks | All arithmetic examples, N=1/N=256, split-DATA state continuity, same-activation reconfiguration reset, invalid zero/high count bits, and simultaneous-header precedence checks passed. | PASS | [python_validation_tlast.log](./python_validation_tlast.log), [test_fir.py](./test_fir.py) |
| C simulation | Complete testbench passed, including same-activation reconfiguration, simultaneous-header precedence, malformed/TLAST errors, unread-input checks, and fresh recoveries. | PASS | [fir_vitis_final.log](./fir_vitis_final.log) |
| Synthesis | Completed for `xc7z020clg484-1`; estimated period 6.973 ns against 10.00 ns. | PASS: clock target met | [fir_vitis_final.log](./fir_vitis_final.log), [fir_accel_csynth.rpt](./fir_proj/solution1/syn/report/fir_accel_csynth.rpt) |
| RTL co-simulation | One complete testbench run passed with port tracing enabled, including the new tests. | PASS | [fir_vitis_final.log](./fir_vitis_final.log), [fir_accel_cosim.rpt](./fir_proj/solution1/sim/report/fir_accel_cosim.rpt) |
| RTL AXI-Stream sidebands | Generated RTL explicitly declares 32-bit input/output TDATA and one-bit input/output TLAST. | PASS | [fir_accel.v](./fir_proj/solution1/syn/verilog/fir_accel.v), [fir_accel_csynth.rpt](./fir_proj/solution1/syn/report/fir_accel_csynth.rpt) |
| T=8/N=256 payload acceptance interval | 256 accepted samples; first cycle 592; last cycle 1355; inclusive elapsed interval 764 cycles. | Measured | [fir_t8_256_final_measurement.txt](./fir_t8_256_final_measurement.txt), [fir_t8_256_final.vcd](./vcd/fir_t8_256_final.vcd), [measure_fir_trace.py](./measure_fir_trace.py) |
| T=8/N=256 throughput target | `256 / 764 = 0.3350785` samples/clock. The interval has 508 TVALID-high/TREADY-low cycles; the inter-transfer gaps are 253×3 cycles and 2×2 cycles. | FAIL: target is >=1 sample/clock | [fir_t8_256_final_measurement.txt](./fir_t8_256_final_measurement.txt), [fir_t8_256_final.vcd](./vcd/fir_t8_256_final.vcd) |
| Total transaction latency | Co-simulation transaction 7 latency 14362 cycles (143.62 us at the 10 ns simulation clock). Trace-defined interval from DATA command acceptance through final result acceptance is 14342 inclusive cycles. | Measured; no separate latency threshold specified | [fir_accel_cosim.rpt](./fir_proj/solution1/sim/report/fir_accel_cosim.rpt), [fir_accel.performance.result.transaction.xml](./fir_proj/solution1/sim/verilog/fir_accel.performance.result.transaction.xml), [fir_t8_256_final_measurement.txt](./fir_t8_256_final_measurement.txt) |
| Payload-read loop II | `VITIS_LOOP_184_5` has a 3-cycle iteration latency, is not pipelined, and has achieved interval `-`; no concrete synthesis II is provided. | not measured by synthesis | [fir_accel_csynth.rpt](./fir_proj/solution1/syn/report/fir_accel_csynth.rpt), [fir_t8_256_final_measurement.txt](./fir_t8_256_final_measurement.txt) |
| Clock period | Estimated 6.973 ns, target 10.00 ns. | PASS | [fir_accel_csynth.rpt](./fir_proj/solution1/syn/report/fir_accel_csynth.rpt) |
| Resources | BRAM_18K=1, DSP=1, FF=1607, LUT=2468, URAM=0. | no target | [fir_accel_csynth.rpt](./fir_proj/solution1/syn/report/fir_accel_csynth.rpt) |

## Verification

Every listed result is from the current `ap_axis` implementation. Each row cites the standalone Python log [python_validation_tlast.log](./python_validation_tlast.log) and/or the latest full hardware log [fir_vitis_final.log](./fir_vitis_final.log).

| Scenario | Python model | C simulation | RTL co-simulation | Evidence |
| --- | --- | --- | --- | --- |
| Example 1: Q1.15 FIR output `[8192,12288,4096]` | PASS | PASS | PASS | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log) |
| Reconfiguration resets history within the same activation; DATA outputs `[8192,12288,4096]` then `[4096,4096]` | PASS; combined outputs `[8192,12288,4096,4096,4096]` | PASS; same-activation CONFIG reset and both response frames checked | PASS; same-activation CONFIG reset and both response frames checked | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [test_fir.py](./test_fir.py), [fir_tb.cpp](./fir_tb.cpp) |
| Halfway rounding `[0,2,0,-2]` | PASS | PASS | PASS | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log) |
| Positive saturation | PASS | PASS | PASS | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log) |
| Negative saturation and boundary checks | PASS | PASS | PASS | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log) |
| T=1 and N=1 | PASS | PASS | PASS | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log) |
| T=8 and N=256 | PASS | PASS | PASS | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_t8_256_final_measurement.txt](./fir_t8_256_final_measurement.txt) |
| Split-DATA state continuity; expected outputs `[8192,12288,4096]` | PASS; two DATA commands in one model stream | PASS; two response headers and both result-burst TLASTs checked | PASS | [python_validation_tlast.log](./python_validation_tlast.log), [test_fir.py](./test_fir.py), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| Invalid N=0 | PASS; model error 5 | PASS; error=5, txid=120, unread=1, output empty; fresh recovery passed | PASS; same status/unread/output and fresh recovery passed | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| Invalid count high bits / above 256 | PASS; model error 5 | PASS; error=5, txid=121, unread=1, output empty; fresh recovery passed | PASS; same status/unread/output and fresh recovery passed | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| Unknown command | PASS; model error 2 and unread input | PASS; error=2, txid=18, unread=1, output empty; fresh recovery passed | PASS; same status/output and fresh recovery passed; unread=1 observed | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| Invalid tap count | PASS; model error 3 and unread input | PASS; error=3, txid=52, unread=1, output empty; fresh recovery passed | PASS; same status/output and fresh recovery passed; unread=1 observed | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| DATA before CONFIG | PASS; model error 4 and unread input | PASS; error=4, txid=86, unread=1, output empty; fresh recovery passed | PASS; same status/output and fresh recovery passed; unread=1 observed | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| Malformed CONFIG header (early TLAST) | PASS; model error 1 and unread input | PASS; error=1, txid=153, unread=1, output empty; separate fresh recovery passed | PASS; same status/unread/output and fresh recovery passed | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| Early payload TLAST | PASS; model error 6 and unread input | PASS; error=6, txid=162, unread=1, output empty; separate fresh recovery passed | PASS; same status/unread/output and fresh recovery passed | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| Missing final payload TLAST | PASS; model error 7 and unread input | PASS; error=7, txid=178, unread=1, output empty; separate fresh recovery passed | PASS; same status/unread/output and fresh recovery passed | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| Precedence: malformed framing over unknown command | PASS; error=1, txid=209, unread=2 | PASS; error=1, txid=209, unread=1, no output; fresh recovery passed | PASS; same status/unread/output and separate fresh recovery passed | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| Precedence: malformed framing over invalid tap count | PASS; error=1, txid=211, unread=2 | PASS; error=1, txid=213, unread=1, no output; fresh recovery passed | PASS; same status/unread/output and separate fresh recovery passed | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| Precedence: DATA before CONFIG over invalid sample count | PASS; error=4, txid=210, unread=3 | PASS; error=4, txid=216, unread=1, no output; fresh recovery passed | PASS; same status/unread/output and separate fresh recovery passed | [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp) |
| Response framing | Sample values compared; model does not model response burst framing | PASS; response header TLAST and result-burst final TLAST checked | PASS; RTL comparison passed with generated TLAST ports | [fir_vitis_final.log](./fir_vitis_final.log), [fir_tb.cpp](./fir_tb.cpp), [fir_accel.v](./fir_proj/solution1/syn/verilog/fir_accel.v) |

The latest physical-TLAST testbench and log support the hardware PASS claims: [fir_tb.cpp](./fir_tb.cpp) constructs streams and checks response frames; [fir_vitis_final.log](./fir_vitis_final.log) contains C-simulation and RTL co-simulation results. The error helper asserts status, unread input, and empty output; each new precedence fault is followed by a fresh successful activation. RTL TLAST declarations are in [fir_accel.v](./fir_proj/solution1/syn/verilog/fir_accel.v); the latest T=8/N=256 handshake trace is [fir_t8_256_final.vcd](./vcd/fir_t8_256_final.vcd).

## Assumptions

- The DATA count is a full 32-bit integer so the protocol represents 1 through 256 without masking invalid high bits.
- Each stream item is one 32-bit protocol word; TKEEP/TSTRB are full-width, so there is no partially filled final data word.
- A command word begins a multiword CONFIG or DATA header burst and therefore has TLAST low; END and one-word unknown commands are treated as TLAST-terminated single-word headers.
- `status_cycles` is the accepted payload sample count, not elapsed RTL cycles; elapsed timing comes from the explicit clock/handshake trace.
- Fresh recovery is modeled as a new kernel activation with newly constructed input and output streams, matching the required host stream-path reset.

## Open Items

- The required one-sample-per-clock throughput target fails at 0.3350785 samples/clock; no optimization was attempted, as directed.
- Synthesis does not report a concrete achieved II for the payload-read loop (`Interval = -`); RTL handshake spacing is measured separately.
- No resource target was specified; resource utilization is reported as measured, with `no target`.

## Run History

| Timestamp | Prompt version | Run | Change and outcome |
| --- | --- | --- | --- |
| 2026-10-28 00:00 | 1 | Initial FIR implementation | Added model, kernel, and testbench; corrected framing/count issues in subsequent runs. |
| 2026-10-28 00:16 | 1 | Functional validation | Python, C simulation, synthesis, and RTL co-simulation passed; timing trace not enabled. |
| 2026-10-28 00:22-00:26 | 1 | Earlier trace attempt | Playback tracing failed because the prior simulator snapshot lacked trace data; timing remained unmeasured in that historical run. |
| 2026-10-28 00:30-00:32 | 1 | Packed-marker tracing | Port tracing exposed that the prior custom struct placed `last` in TDATA rather than providing AXI TLAST. That interface was superseded by this run. |
| 2026-10-28 00:52-00:53 | 1; choices later documented in 2 | Physical-TLAST validation | Changed kernel and testbench streams to `ap_axis<32,0,0,0>`, added response-frame checks and fresh recoveries for malformed/early/missing-TLAST errors, and updated the Python count/TLAST model. Independent Python checks, full C simulation, synthesis, and one complete traced RTL co-simulation passed. Generated RTL contains input/output TLAST ports. Trace measured 256 samples over 764 cycles (`0.3350785` samples/clock), so throughput is FAIL. Evidence: [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_tlast.log](./fir_vitis_tlast.log), [fir_accel.v](./fir_proj/solution1/syn/verilog/fir_accel.v), [fir_accel_csynth.rpt](./fir_proj/solution1/syn/report/fir_accel_csynth.rpt), and [fir_t8_256_tlast.vcd](./vcd/fir_t8_256_tlast.vcd). |
| 2026-10-28 01:03 | 2 | Standalone Python split-DATA check | Added the two-DATA-command state-continuity vector and reran only `test_fir.py`; it passed with `[8192,12288,4096]`. No hardware files changed, so C simulation, synthesis, and RTL co-simulation were not rerun. Evidence: [test_fir.py](./test_fir.py), [python_validation_tlast.log](./python_validation_tlast.log). |
| 2026-10-28 01:12-01:17 | 2 | Reconfiguration and precedence tests | Added standalone Python and C++ tests for CONFIG resetting history within one activation and simultaneous header-fault precedence; strengthened error helper unread checks. Python, C simulation, synthesis, and one traced RTL co-simulation passed. Refreshed trace: 256 accepted samples, 764 cycles, `0.3350785` samples/clock (throughput remains FAIL). Synthesis II remains unreported. Evidence: [python_validation_tlast.log](./python_validation_tlast.log), [fir_vitis_final.log](./fir_vitis_final.log), [fir_accel_csynth.rpt](./fir_proj/solution1/syn/report/fir_accel_csynth.rpt), [fir_accel_cosim.rpt](./fir_proj/solution1/sim/report/fir_accel_cosim.rpt), [fir_t8_256_final.vcd](./vcd/fir_t8_256_final.vcd), and [fir_t8_256_final_measurement.txt](./fir_t8_256_final_measurement.txt). |
