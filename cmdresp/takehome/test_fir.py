import numpy as np

from fir import EXAMPLE_1, EXAMPLE_3, EXAMPLE_4, EXAMPLE_5, FirCmdType, FirError, fir_eval, fir_stream_model


def check_example(label, coeffs, samples, expected):
    actual = fir_eval(coeffs, samples)
    assert actual == expected, f"{label}: expected {expected}, got {actual}"
    print(f"PASS {label}: {actual}")


def check_error_case(label, bursts, expected_error, expected_tx, expect_unread):
    result = fir_stream_model(bursts)
    assert result.halted == 1, f"{label}: expected halted=1 got {result.halted}"
    assert result.error == expected_error, f"{label}: expected error {expected_error} got {result.error}"
    assert result.tx_id == expected_tx, f"{label}: expected tx {expected_tx} got {result.tx_id}"
    assert len(result.out) == 0, f"{label}: expected no output got {result.out}"
    assert (len(result.unread) > 0) == expect_unread, (
        f"{label}: unread expectation mismatch: expect_unread={expect_unread}, unread={len(result.unread)}"
    )
    print(f"PASS {label}: err={expected_error} tx={expected_tx} unread={len(result.unread)}")


def make_cfg_word(tx, taps):
    return (taps << 24) | (tx << 8) | FirCmdType.CONFIG


def make_data_word(tx):
    return (tx << 8) | FirCmdType.DATA


if __name__ == "__main__":
    check_example("example_1", EXAMPLE_1["coeffs"], EXAMPLE_1["x"], EXAMPLE_1["expected"])
    check_example("example_3", EXAMPLE_3["coeffs"], EXAMPLE_3["x"], EXAMPLE_3["expected"])
    check_example("example_4", EXAMPLE_4["coeffs"], EXAMPLE_4["x"], EXAMPLE_4["expected"])
    check_example("example_5", EXAMPLE_5["coeffs"], EXAMPLE_5["x"], EXAMPLE_5["expected"])

    # Count encoding coverage: valid N=1, valid N=256, and invalid N=0.
    cfg = [(make_cfg_word(0x44, 1), False), (16384, False), (16384, False), (16384, False), (16384, False),
           (16384, False), (16384, False), (16384, False), (16384, True)]
    data_1 = [(make_data_word(0x55), False), (1, True), (16384, True)]
    result = fir_stream_model(cfg + data_1)
    assert result.error == int(FirError.NO_ERROR), f"N=1 invalid: {result.error}"
    assert result.out == [8192], f"N=1 wrong output: {result.out}"
    print("PASS count_n1")

    cfg_256 = [(make_cfg_word(0x66, 1), False), (16384, False), (16384, False), (16384, False), (16384, False),
               (16384, False), (16384, False), (16384, False), (16384, True)]
    data_256 = [(make_data_word(0x77), False), (256, True)] + [(0, False) for _ in range(255)] + [(0, True)]
    result = fir_stream_model(cfg_256 + data_256)
    assert result.error == int(FirError.NO_ERROR), f"N=256 invalid: {result.error}"
    assert len(result.out) == 256, f"N=256 wrong length: {len(result.out)}"
    print("PASS count_n256")

    cfg_split = [(make_cfg_word(0x20, 2), False), (16384, False), (16384, False), (16384, False),
                 (16384, False), (16384, False), (16384, False), (16384, False), (16384, True)]
    data_split_1 = [(make_data_word(0x21), False), (1, True), (16384, True)]
    data_split_2 = [(make_data_word(0x22), False), (2, True), (8192, False), (0, True)]
    result = fir_stream_model(cfg_split + data_split_1 + data_split_2)
    assert result.error == int(FirError.NO_ERROR), f"split DATA state check failed: {result.error}"
    assert result.out == [8192, 12288, 4096], f"split DATA state wrong: {result.out}"
    assert result.unread == [], f"split DATA left unread words: {result.unread}"
    print(f"PASS split_data_state_continuity: {result.out}")

    cfg_reconfigure = [
        (make_cfg_word(0x30, 2), False),
        *[(16384, False) for _ in range(7)],
        (16384, True),
    ]
    data_before_reconfigure = [(make_data_word(0x31), False), (3, True),
                               (16384, False), (8192, False), (0, True)]
    cfg_reconfigure_again = [
        (make_cfg_word(0x32, 2), False),
        *[(16384, False) for _ in range(7)],
        (16384, True),
    ]
    data_after_reconfigure = [(make_data_word(0x33), False), (2, True), (8192, False), (0, True)]
    result = fir_stream_model(
        cfg_reconfigure + data_before_reconfigure + cfg_reconfigure_again + data_after_reconfigure
    )
    assert result.error == int(FirError.NO_ERROR), f"reconfiguration check failed: {result.error}"
    assert result.out == [8192, 12288, 4096, 4096, 4096], (
        f"reconfiguration did not reset history: {result.out}"
    )
    assert result.unread == [], f"reconfiguration left unread words: {result.unread}"
    print(f"PASS reconfiguration_resets_history: {result.out}")

    invalid_zero = [(make_cfg_word(0x88, 1), False), (16384, False), (16384, False), (16384, False), (16384, False),
                    (16384, False), (16384, False), (16384, False), (16384, True),
                    (make_data_word(0x88), False), (0, True), (16384, True)]
    result = fir_stream_model(invalid_zero)
    assert result.error == int(FirError.INVALID_SAMPLE_COUNT), f"invalid count check got {result.error}"
    print("PASS count_invalid_zero")

    invalid_high_bits = [(make_cfg_word(0x89, 1), False), (16384, False), (16384, False), (16384, False),
                         (16384, False), (16384, False), (16384, False), (16384, False), (16384, True),
                         (make_data_word(0x89), False), (0x100000001, True), (16384, True)]
    result = fir_stream_model(invalid_high_bits)
    assert result.error == int(FirError.INVALID_SAMPLE_COUNT), (
        f"invalid high count bits check got {result.error}"
    )
    print("PASS count_invalid_high_bits")

    # Error path validation using the serialized-burst model.
    check_error_case(
        "err_unknown_command",
        [(0x12 << 8 | 0xFF, True), (0x00000002, False), (0x00000000, False)],
        int(FirError.UNKNOWN_COMMAND),
        0x12,
        True,
    )

    check_error_case(
        "err_malformed_header",
        [(make_cfg_word(0x34, 1), False), (0x00004000, False), (0x00000002, False)],
        int(FirError.MALFORMED_HEADER),
        0x34,
        True,
    )

    check_error_case(
        "precedence_malformed_over_unknown",
        [(((0xD1 << 8) | 0xFF), False), (int(FirCmdType.END), True)],
        int(FirError.MALFORMED_HEADER),
        0xD1,
        True,
    )

    check_error_case(
        "precedence_malformed_over_invalid_tap",
        [(make_cfg_word(0xD3, 9), True), (16384, True)],
        int(FirError.MALFORMED_HEADER),
        0xD3,
        True,
    )

    check_error_case(
        "precedence_data_before_config_over_invalid_count",
        [(make_data_word(0xD2), False), (0, True), (1, True)],
        int(FirError.DATA_BEFORE_CONFIG),
        0xD2,
        True,
    )

    check_error_case(
        "err_invalid_tap_count",
        [(make_cfg_word(0x34, 9), False), (0x00004000, False), (0x00000000, False), (0x00000000, False),
         (0x00000000, False), (0x00000000, False), (0x00000000, False), (0x00000000, False),
         (0x00000000, False), (0x00000000, False)],
        int(FirError.INVALID_TAP_COUNT),
        0x34,
        True,
    )

    check_error_case(
        "err_data_before_config",
        [(make_data_word(0x56), False), (3, True), (0x0001, False), (0x0002, False), (0x0003, True)],
        int(FirError.DATA_BEFORE_CONFIG),
        0x56,
        True,
    )

    check_error_case(
        "err_invalid_sample_count",
        [(make_cfg_word(0x78, 1), False), (16384, False), (16384, False), (16384, False), (16384, False),
         (16384, False), (16384, False), (16384, False), (16384, True),
         (make_data_word(0x78), False), (0, True), (0x0001, True)],
        int(FirError.INVALID_SAMPLE_COUNT),
        0x78,
        True,
    )

    cfg_2tap = [(make_cfg_word(0xA4, 2), False), (16384, False), (16384, False), (16384, False), (16384, False),
                (16384, False), (16384, False), (16384, False), (16384, True)]
    bursts = cfg_2tap + [(make_data_word(0xA4), False), (2, True), (16384, True), (8192, False)]
    result = fir_stream_model(bursts)
    assert result.error == int(FirError.EARLY_TLAST), f"expected EARLY_TLAST got {result.error}"
    print("PASS err_early_tlast")

    cfg_2tap = [(make_cfg_word(0xA5, 2), False), (16384, False), (16384, False), (16384, False), (16384, False),
                (16384, False), (16384, False), (16384, False), (16384, True)]
    bursts = cfg_2tap + [(make_data_word(0xA5), False), (2, True), (16384, False), (8192, False)]
    result = fir_stream_model(bursts)
    assert result.error == int(FirError.MISSING_TLAST), f"expected MISSING_TLAST got {result.error}"
    print("PASS err_missing_tlast")

    # Boundary checks around the negative-saturation limit.
    cfg_neg = [(make_cfg_word(0xB1, 1), False), (32767, False), (32767, False), (32767, False), (32767, False),
               (32767, False), (32767, False), (32767, False), (32767, True)]
    result = fir_stream_model(cfg_neg + [(make_data_word(0xB1), False), (1, True), (-32768, True)])
    assert result.error == int(FirError.NO_ERROR), f"negative boundary above floor should be valid: {result.error}"
    assert result.out == [-32767], f"negative boundary above floor wrong: {result.out}"
    print("PASS neg_boundary_above_floor")

    cfg_neg2 = [(make_cfg_word(0xB2, 2), False), (32767, False), (32767, False), (32767, False), (32767, False),
                (32767, False), (32767, False), (32767, False), (32767, True)]
    result = fir_stream_model(cfg_neg2 + [(make_data_word(0xB2), False), (2, True), (-32768, False), (-32768, True)])
    assert result.error == int(FirError.NO_ERROR), f"negative saturation should be valid: {result.error}"
    assert result.out == [-32767, -32768], f"negative saturation wrong: {result.out}"
    print("PASS neg_boundary_below_floor")
