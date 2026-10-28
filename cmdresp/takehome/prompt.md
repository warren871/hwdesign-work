# Streaming Fixed-Point FIR Filter Accelerator

Build a Vitis HLS kernel in this folder following the Waveflow
`stream_inband` example and the course command-response pattern.
Where they differ, follow the course pattern.

Write a bit-exact Python model, a Vitis HLS kernel, and a testbench.
Compare the Python model, C simulation, and RTL co-simulation bit for
bit. Use the FPGA part and clock target from the reference example.

## Operation

Implement a causal FIR filter with a programmable number of taps T:

    y[n] = sum(h[k] * x[n-k]),  k = 0, ..., T-1

The tap count must satisfy 1 <= T <= 8.

A CONFIG command provides T and eight coefficient fields, h[0] through
h[7]. Only the first T coefficients are used.

At CONFIG, initialize the filter's previous input samples to zero. A
DATA command processes its input samples in order and produces one
output per input. The delay line carries across consecutive DATA
commands within the same activation.

A second CONFIG replaces the tap count and coefficients and clears the
delay line, even if the new configuration is identical to the old one.

END stops the kernel without flushing a filter tail. Each new
activation starts unconfigured with no retained filter history.

## Data format and arithmetic

Inputs, coefficients, and outputs use signed 16-bit two's-complement
Q1.15 format. A stored integer q represents q / 32768.

For every output:

1. Multiply the signed raw input and coefficient integers exactly.
2. Accumulate all products in a signed accumulator of at least
   35 bits. Do not truncate, round, saturate, or overflow between taps.
3. Divide the final accumulator by 32768 to return to Q1.15.
4. Round to the nearest integer; for exact halfway cases, round to the
   even integer. Apply this rule to negative values as well.
5. Saturate the result to the signed 16-bit range [-32768, 32767].

The Python model and HLS implementation must use the same rules and
match bit for bit. Do not rely on signed integer overflow.

## Requirements

- Support 1 to 256 input samples per DATA command.
- Buffer and validate the entire DATA payload before emitting its
  response, so a faulty payload produces no partial response.
- Target: for a valid DATA command with 256 samples and T=8, accept
  at least one 16-bit input sample per clock during the payload-reading
  phase, with no testbench backpressure. Measure the rate as the sample
  count divided by the inclusive cycle count from the first accepted
  sample to the last. Clearly state how packed samples are counted.
- Measure the actual latency and throughput from co-simulation, and
  report achieved initiation interval, estimated clock period, and
  resource utilization from synthesis.
- The clock target is met if Vitis's estimated clock period is below
  10 ns. A clock-uncertainty warning alone is not a failure.
- Report a missed target as FAIL. Do not repeatedly restructure the
  design to close timing, and do not create a Python timing model.

## Commands and responses

Use the command-response pattern:

- CONFIG: carries T and all eight coefficient fields in its header.
  It has no separate payload and produces no response.
- DATA: carries the sample count N in its header, followed by a
  separate input-data burst. A valid command produces a response
  header echoing the transaction ID, followed by N output samples.
- END: produces no response and causes normal return.

Each command header is a separate TLAST-terminated burst. The DATA
payload, response header, and result payload are also separate bursts,
each terminated with TLAST.

Commands, coefficients, and samples travel in-band on the input stream.
AXI-Lite carries control and status only.

You may choose command codes, header layouts, and packing. Document
them consistently in the model, kernel, testbench, and results. Explain
how a partially filled final data word is handled.

Validate header parameters before reading a DATA payload.

## Errors

Errors go in AXI-Lite status registers: halted, error code, and the
offending transaction ID.

On an error, close any output burst already started with TLAST, set
the status registers, and return without reading further input. Do not
drain the input stream. The host reads status, resets the stream path,
and starts a fresh activation. Input remaining after an error is
undefined until the host resets the stream path.

Use distinct error codes for:

- Malformed command-header framing.
- Unknown command.
- Invalid tap count.
- DATA before CONFIG.
- Invalid sample count.
- Early TLAST on the DATA payload.
- Missing TLAST on the expected final DATA payload word.

Valid tap counts are 1 through 8; valid sample counts are 1 through 256.

For header faults, apply this precedence:

1. Malformed header framing.
2. Unknown command.
3. DATA before CONFIG.
4. Invalid command-specific parameters.

Stop on the AXI word that reveals the error. A faulty DATA command
produces no response. Earlier completed responses remain valid.

If a malformed header does not contain a complete transaction ID,
report zero as the transaction ID. Define and document any remaining
choices about unused header fields and packed words.

## Verification

Independently check the Python model against a separate integer
reference and the following hand-computed raw Q1.15 examples:

1. With h = [16384, 16384] and zero history:

       x = [16384, 8192, 0]
       y = [8192, 12288, 4096]

2. Split the same input across two DATA commands:

       DATA 1: x = [16384]    -> y = [8192]
       DATA 2: x = [8192, 0] -> y = [12288, 4096]

   This checks that state carries across commands.

3. Reconfigure with the same taps before DATA 2. The history is reset:

       DATA: x = [8192, 0] -> y = [4096, 4096]

4. With h = [16384], test halfway rounding:

       x = [1, 3, -1, -3]
       y = [0, 2, 0, -2]

5. Test positive saturation with h = [-32768, -32768]:

       x = [-32768, -32768]
       y = [32767, 32767]

Also test negative saturation independently.

Test T=1 and T=8; N=1 and N=256; zero and signed-extreme values;
rounding just below and above halfway; both saturation limits;
state continuity; reconfiguration; multiple DATA commands; and END.

For each error, check status, transaction ID, output, TLAST, and
whether later queued input words remain unread. Follow every error
activation with a separate successful activation using fresh streams.
Test simultaneous header faults to verify error precedence, as well
as early and missing TLAST. Verify that configuration and history do
not survive a new activation.

Compare the model, C simulation, and co-simulation bit for bit. Run
co-simulation once: use one testbench run that walks through all
scenarios in order, calling the kernel once per activation with fresh
streams, including every error and its fresh recovery activation.

If a simulation framework cannot express a malformed burst, document
the limitation and verify it at another applicable level. Do not
report an unrun check as PASS.

## results.md

Write results.md with these seven sections:

- Interface: command and response layouts, word packing, status
  registers, and error codes.
- Design: loop structure, filter state, and how the throughput target
  is reached or why it is not reached.
- Targets: each required target, measured value, PASS or FAIL, and
  evidence file. Report latency and throughput from co-simulation and
  clock period, II, and resources from synthesis; write "no target"
  where none was specified.
- Verification: the independent model checks and one row per scenario
  showing model, C-simulation, and co-simulation results. For each
  error, include status and fresh-recovery outcome.
- Assumptions: every consequential ambiguity and the choice made,
  excluding interface details already documented under Interface.
- Open Items: unanswered questions, failures, and work not completed,
  or "None."
- Run History: one row per run with the prompt version, changes made,
  and checks passed.

Every number and PASS/FAIL in Targets and Verification must name its
source file. Keep the cited logs, comparison outputs, and synthesis
reports in the project folder so they can be committed and reviewed.

## Revision history

| Version | Change | Why |
| --- | --- | --- |
| 1 | Initial FIR specification | Use a programmable, stateful FIR filter for the take-home project. |
| 2 | Clarified the host-visible stream framing: use a physical AXI-Stream TLAST sideband; CONFIG carries its command and all eight coefficient words in one TLAST-terminated header burst; DATA carries its command and a full 32-bit count word (valid 1..256) in one TLAST-terminated header burst, followed by a separate sample burst; responses use separate TLAST-terminated header and result bursts. | Make the wire-level header boundaries, N=256 count encoding, and TLAST placement unambiguous for host software. This records implementation choices without changing the requested FIR behavior or the one-sample-per-clock throughput target. |
