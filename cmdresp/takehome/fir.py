from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from typing import Iterable, Sequence

import numpy as np

Q15_SCALE = 32768
Q15_HALF = 16384
INT16_MIN = -32768
INT16_MAX = 32767


class FirCmdType(IntEnum):
    CONFIG = 0
    DATA = 1
    END = 2


class FirError(IntEnum):
    NO_ERROR = 0
    MALFORMED_HEADER = 1
    UNKNOWN_COMMAND = 2
    INVALID_TAP_COUNT = 3
    DATA_BEFORE_CONFIG = 4
    INVALID_SAMPLE_COUNT = 5
    EARLY_TLAST = 6
    MISSING_TLAST = 7


@dataclass
class FirResult:
    out: list[int]
    halted: int = 0
    error: int = int(FirError.NO_ERROR)
    tx_id: int = 0
    unread: list[tuple[int, bool]] | None = None


def q15_round_even(acc: int) -> int:
    """Round acc/32768 to the nearest integer, with halves to even."""
    sign = -1 if acc < 0 else 1
    mag = abs(acc)
    quotient = mag // Q15_SCALE
    remainder = mag % Q15_SCALE
    if remainder > Q15_HALF or (remainder == Q15_HALF and (quotient & 1)):
        quotient += 1
    result = quotient if sign > 0 else -quotient
    if result < INT16_MIN:
        return INT16_MIN
    if result > INT16_MAX:
        return INT16_MAX
    return int(result)


def q15_mul_add(coeffs: Sequence[int], x_seq: Sequence[int]) -> list[int]:
    """Bit-exact order for one command: exact products, 35-bit accumulator, divide, round, saturate."""
    out: list[int] = []
    for x in x_seq:
        acc = 0
        for coeff in coeffs:
            acc += int(np.int64(x)) * int(np.int64(coeff))
        out.append(q15_round_even(acc))
    return out


def fir_eval(coeffs: Sequence[int], x: Sequence[int]) -> list[int]:
    """Causal FIR using the command-response state update with a programmable tap length."""
    taps = list(coeffs)
    T = len(taps)
    history = [0] * 8
    out: list[int] = []
    for sample in x:
        for i in range(T - 1, 0, -1):
            history[i] = history[i - 1]
        history[0] = int(sample)
        acc = 0
        for k in range(T):
            acc += int(np.int64(history[k])) * int(np.int64(taps[k]))
        out.append(q15_round_even(acc))
    return out


def _sample_from_word(word: int) -> int:
    value = int(word & 0xFFFF)
    if value & 0x8000:
        value -= 0x10000
    return value


def fir_stream_model(bursts: Sequence[tuple[int, bool]], word_bw: int = 32) -> FirResult:
    """Model the in-band FIR protocol and error ordering described in prompt.md.

    Header layout (revised to support N=256 cleanly):
      - CONFIG: command word followed by eight coefficient words in the same TLAST-terminated burst.
      - DATA: command word followed by one 32-bit count word (valid range 1..256) and then the
        separate sample burst. Each item’s boolean is the physical AXI-Stream TLAST sideband.
    """
    if word_bw != 32:
        raise ValueError("This prototype targets 32-bit stream words only.")

    active_taps: list[int] = []
    history = [0] * 8
    results: list[int] = []
    pos = 0
    configured = False

    while pos < len(bursts):
        word, command_last = bursts[pos]
        cmd_type = word & 0xFF
        tx_id = (word >> 8) & 0xFFFF
        field = (word >> 24) & 0xFF

        if cmd_type in (FirCmdType.CONFIG, FirCmdType.DATA) and command_last:
            return FirResult(results, halted=1, error=int(FirError.MALFORMED_HEADER), tx_id=tx_id, unread=bursts[pos:])
        if cmd_type == FirCmdType.END and not command_last:
            return FirResult(results, halted=1, error=int(FirError.MALFORMED_HEADER), tx_id=tx_id, unread=bursts[pos:])
        if cmd_type not in (FirCmdType.CONFIG, FirCmdType.DATA, FirCmdType.END) and not command_last:
            return FirResult(results, halted=1, error=int(FirError.MALFORMED_HEADER), tx_id=tx_id, unread=bursts[pos:])

        if cmd_type == FirCmdType.CONFIG:
            if pos + 1 + 8 > len(bursts):
                return FirResult(results, halted=1, error=int(FirError.MALFORMED_HEADER), tx_id=tx_id, unread=bursts[pos:])
            if field < 1 or field > 8:
                return FirResult(results, halted=1, error=int(FirError.INVALID_TAP_COUNT), tx_id=tx_id, unread=bursts[pos:])
            coeffs: list[int] = []
            for j in range(8):
                coeff_word, coeff_last = bursts[pos + 1 + j]
                if j < 7 and coeff_last:
                    return FirResult(results, halted=1, error=int(FirError.MALFORMED_HEADER), tx_id=tx_id, unread=bursts[pos:])
                if j == 7 and not coeff_last:
                    return FirResult(results, halted=1, error=int(FirError.MALFORMED_HEADER), tx_id=tx_id, unread=bursts[pos:])
                coeffs.append(_sample_from_word(coeff_word))
            active_taps = coeffs[:field]
            history = [0] * 8
            configured = True
            pos += 1 + 8
            continue

        if cmd_type == FirCmdType.END:
            return FirResult(results, halted=0, error=int(FirError.NO_ERROR), tx_id=0, unread=[])

        if cmd_type == FirCmdType.DATA:
            if not configured:
                return FirResult(results, halted=1, error=int(FirError.DATA_BEFORE_CONFIG), tx_id=tx_id, unread=bursts[pos:])
            if pos + 1 >= len(bursts):
                return FirResult(results, halted=1, error=int(FirError.MALFORMED_HEADER), tx_id=tx_id, unread=bursts[pos:])
            count_word, count_last = bursts[pos + 1]
            if not count_last:
                return FirResult(results, halted=1, error=int(FirError.MALFORMED_HEADER), tx_id=tx_id, unread=bursts[pos:])
            field = int(count_word)
            if field < 1 or field > 256:
                return FirResult(results, halted=1, error=int(FirError.INVALID_SAMPLE_COUNT), tx_id=tx_id, unread=bursts[pos:])
            payload_start = pos + 2
            payload_end = payload_start + field
            if payload_end > len(bursts):
                return FirResult(results, halted=1, error=int(FirError.MISSING_TLAST), tx_id=tx_id, unread=bursts[pos:])
            for idx in range(field):
                sample_word, sample_tlast = bursts[payload_start + idx]
                if sample_tlast and idx != field - 1:
                    return FirResult(results, halted=1, error=int(FirError.EARLY_TLAST), tx_id=tx_id, unread=bursts[pos:])
            if not bursts[payload_end - 1][1]:
                return FirResult(results, halted=1, error=int(FirError.MISSING_TLAST), tx_id=tx_id, unread=bursts[pos:])

            samples = [_sample_from_word(bursts[payload_start + idx][0]) for idx in range(field)]
            for sample in samples:
                for i in range(len(active_taps) - 1, 0, -1):
                    history[i] = history[i - 1]
                history[0] = int(sample)
                acc = 0
                for tap_index, tap in enumerate(active_taps):
                    acc += int(np.int64(history[tap_index])) * int(np.int64(tap))
                results.append(q15_round_even(acc))
            pos = payload_end
            continue

        return FirResult(results, halted=1, error=int(FirError.UNKNOWN_COMMAND), tx_id=tx_id, unread=bursts[pos:])

    return FirResult(results, halted=0, error=int(FirError.NO_ERROR), tx_id=0, unread=[])


def parse_words_from_u32(words: Sequence[int]) -> list[int]:
    return [int(w & 0xFFFFFFFF) for w in words]


# Example values from the prompt.
EXAMPLE_1 = {
    "coeffs": [16384, 16384],
    "x": [16384, 8192, 0],
    "expected": [8192, 12288, 4096],
}
EXAMPLE_2 = {
    "coeffs": [16384, 16384],
    "x": [16384, 8192, 0],
    "expected_split": [8192, 12288, 4096],
}
EXAMPLE_3 = {
    "coeffs": [16384, 16384],
    "x": [8192, 0],
    "expected": [4096, 4096],
}
EXAMPLE_4 = {
    "coeffs": [16384],
    "x": [1, 3, -1, -3],
    "expected": [0, 2, 0, -2],
}
EXAMPLE_5 = {
    "coeffs": [-32768, -32768],
    "x": [-32768, -32768],
    "expected": [32767, 32767],
}
