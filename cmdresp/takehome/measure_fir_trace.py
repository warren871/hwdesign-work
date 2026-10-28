#!/usr/bin/env python3
"""Extract the T=8, N=256 AXI-Stream timing from a Vitis port-level VCD."""

import re
import sys
from collections import Counter
from pathlib import Path


def parse_vcd(path: Path):
    scopes = []
    signals = {}
    timestamps = []
    time_ps = None
    changes = []

    with path.open(encoding="utf-8") as trace:
        for line in trace:
            item = line.strip()
            if item.startswith("$scope"):
                scopes.append(item.split()[2])
            elif item.startswith("$upscope"):
                scopes.pop()
            elif item.startswith("$var"):
                fields = item.split()
                signals["/".join(scopes + [fields[4]])] = fields[3]
            elif item.startswith("#"):
                if time_ps is not None:
                    timestamps.append((time_ps, changes))
                time_ps = int(item[1:])
                changes = []
            elif item and not item.startswith("$"):
                if item[0] in "bB":
                    value, identifier = item[1:].split()
                else:
                    value, identifier = item[0], item[1:]
                changes.append((identifier, value.lower()))

    if time_ps is not None:
        timestamps.append((time_ps, changes))
    return signals, timestamps


def value(state, identifier):
    raw = state.get(identifier, "x")
    if raw and set(raw) <= {"0", "1"}:
        return int(raw, 2)
    return None


def main():
    vcd_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("vcd/fir_t8_256_tlast.vcd")
    signals, timestamps = parse_vcd(vcd_path)
    signal_ids = {name.rsplit("/", 1)[-1]: identifier for name, identifier in signals.items()}
    required = (
        "ap_clk",
        "in_r_TDATA",
        "in_r_TVALID",
        "in_r_TREADY",
        "in_r_TLAST",
        "out_r_TDATA",
        "out_r_TVALID",
        "out_r_TREADY",
        "out_r_TLAST",
    )
    missing = [name for name in required if name not in signal_ids]
    if missing:
        raise RuntimeError(f"VCD is missing required AXI-Stream signals: {missing}")

    state = {}
    cycle = 0
    input_transfers = []
    output_transfers = []
    for time_ps, changes in timestamps:
        clk_before = value(state, signal_ids["ap_clk"])
        state.update(changes)
        clk_after = value(state, signal_ids["ap_clk"])
        if clk_before != 0 or clk_after != 1:
            continue
        cycle += 1
        for prefix, transfers in (("in_r", input_transfers), ("out_r", output_transfers)):
            data = value(state, signal_ids[f"{prefix}_TDATA"])
            valid = value(state, signal_ids[f"{prefix}_TVALID"])
            ready = value(state, signal_ids[f"{prefix}_TREADY"])
            last = value(state, signal_ids[f"{prefix}_TLAST"])
            if valid == 1 and ready == 1 and data is not None and last is not None:
                transfers.append((cycle, time_ps, data, last))

    config = next(
        event
        for event in input_transfers
        if event[2] & 0xFF == 0 and (event[2] >> 8) & 0xFFFF == 15
    )
    if (config[2] >> 24) & 0xFF != 8:
        raise RuntimeError("The selected activation is not configured for T=8.")
    command_index = next(
        index
        for index, event in enumerate(input_transfers)
        if event[2] & 0xFF == 1 and (event[2] >> 8) & 0xFFFF == 16
    )
    command = input_transfers[command_index]
    count_header = input_transfers[command_index + 1]
    sample_count = count_header[2]
    payload = input_transfers[command_index + 2 : command_index + 2 + sample_count]
    if sample_count != 256 or count_header[3] != 1 or len(payload) != sample_count:
        raise RuntimeError("The selected DATA tx_id=16 burst is not a complete N=256 payload.")
    if any(event[3] for event in payload[:-1]) or payload[-1][3] != 1:
        raise RuntimeError("The input payload TLAST position does not match N=256.")

    response_index = next(
        index
        for index, event in enumerate(output_transfers)
        if event[0] > command[0] and event[2] == 16 and event[3] == 1
    )
    response_header = output_transfers[response_index]
    results = output_transfers[response_index + 1 : response_index + 1 + sample_count]
    if len(results) != sample_count or any(event[3] for event in results[:-1]) or results[-1][3] != 1:
        raise RuntimeError("The output result burst/TLAST positions do not match N=256.")

    first_cycle = payload[0][0]
    last_cycle = payload[-1][0]
    elapsed = last_cycle - first_cycle + 1
    gaps = Counter(right[0] - left[0] for left, right in zip(payload, payload[1:]))
    transaction_report = Path("fir_proj/solution1/sim/verilog/fir_accel.performance.result.transaction.xml")
    transaction_text = transaction_report.read_text(encoding="utf-8")
    match = re.search(r"transaction\s+7:\s+(\d+)\s+(\d+)", transaction_text)
    if not match:
        raise RuntimeError(f"Could not find transaction 7 in {transaction_report}.")

    print(f"VCD: {vcd_path}")
    print("AXI signals: ap_clk, in_r_TVALID/TREADY/TDATA/TLAST, out_r_TVALID/TREADY/TDATA/TLAST")
    print(f"CONFIG tx_id=15, T=8 at cycle={config[0]}; DATA command cycle={command[0]}, N={sample_count}, count TLAST={count_header[3]}")
    print(f"Payload accepted={len(payload)}, first_cycle={first_cycle}, last_cycle={last_cycle}, inclusive_cycles={elapsed}")
    print(f"Measured payload rate={sample_count}/{elapsed}={sample_count / elapsed:.7f} samples/clock")
    print(f"Input TLAST: count header=cycle {count_header[0]}; payload only final sample=cycle {payload[-1][0]}")
    print(
        f"Response header: tx_id={response_header[2]} TLAST={response_header[3]} "
        f"cycle={response_header[0]}"
    )
    print(
        f"Result TLAST: only final of {len(results)} words, cycle={results[-1][0]}; "
        f"DATA-command-to-final-result-inclusive={results[-1][0] - command[0] + 1} cycles"
    )
    print(f"Payload inter-transfer gaps={dict(sorted(gaps.items()))}")
    print(f"Vitis transaction 7 latency={match.group(1)} cycles, interval={match.group(2)} cycles")


if __name__ == "__main__":
    main()
