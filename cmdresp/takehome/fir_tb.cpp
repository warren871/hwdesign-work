#include <ap_int.h>
#include <ap_axi_sdata.h>
#include <hls_stream.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

#define CMD_CONFIG 0
#define CMD_DATA 1
#define CMD_END 2
#define ERR_NO_ERROR 0
#define ERR_MALFORMED_HEADER 1
#define ERR_UNKNOWN_COMMAND 2
#define ERR_INVALID_TAP_COUNT 3
#define ERR_DATA_BEFORE_CONFIG 4
#define ERR_INVALID_SAMPLE_COUNT 5
#define ERR_EARLY_TLAST 6
#define ERR_MISSING_TLAST 7
#define MAX_TAPS 8
#define MAX_SAMPLES 256

typedef ap_axis<32, 0, 0, 0> axis_word;

typedef hls::stream<axis_word> axis_stream_t;

typedef ap_int<16> q15_t;

void fir_accel(axis_stream_t &in, axis_stream_t &out,
               ap_uint<32> &status_halted,
               ap_uint<32> &status_error,
               ap_uint<32> &status_txid,
               ap_uint<32> &status_cycles);

static axis_word make_axis(ap_uint<32> data, bool last) {
    axis_word w;
    w.data = data;
    w.last = last ? 1 : 0;
    w.keep = 0xF;
    w.strb = 0xF;
    return w;
}

static ap_uint<32> make_cmd(uint8_t cmd, uint16_t tx, uint8_t field) {
    ap_uint<32> w = 0;
    w.range(7, 0) = cmd;
    w.range(23, 8) = tx;
    w.range(31, 24) = field;
    return w;
}

static ap_uint<32> make_coeff(int16_t v) { ap_uint<32> w = 0; w.range(15, 0) = (ap_uint<16>)v; return w; }
static ap_uint<32> make_sample(int16_t v) { ap_uint<32> w = 0; w.range(15, 0) = (ap_uint<16>)v; return w; }
static ap_uint<32> make_count(uint32_t n) { return n; }

static int round_q15_even(long long acc) {
    long long q = acc / 32768LL;
    long long rem = acc % 32768LL;
    long long abs_rem = rem < 0 ? -rem : rem;
    if (abs_rem > 16384LL || (abs_rem == 16384LL && (q & 1LL))) {
        if (acc >= 0) ++q;
        else --q;
    }
    if (q > 32767) q = 32767;
    if (q < -32768) q = -32768;
    return (int)q;
}

static std::vector<int16_t> fir_reference(const std::vector<int16_t>& coeffs, const std::vector<int16_t>& samples) {
    std::vector<int16_t> out;
    std::vector<int16_t> history(8, 0);
    for (size_t i = 0; i < samples.size(); ++i) {
        for (int j = (int)coeffs.size() - 1; j > 0; --j) {
            history[j] = history[j - 1];
        }
        history[0] = samples[i];
        long long acc = 0;
        for (size_t k = 0; k < coeffs.size(); ++k) {
            acc += (long long)history[k] * (long long)coeffs[k];
        }
        out.push_back((int16_t)round_q15_even(acc));
    }
    return out;
}

static bool check_vec(const std::vector<int16_t>& got, const std::vector<int16_t>& exp, const char* label) {
    if (got.size() != exp.size()) {
        printf("FAIL %s: size mismatch got=%zu exp=%zu\n", label, got.size(), exp.size());
        return false;
    }
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i] != exp[i]) {
            printf("FAIL %s[%zu]: got=%d exp=%d\n", label, i, (int)got[i], (int)exp[i]);
            return false;
        }
    }
    printf("PASS %s\n", label);
    return true;
}

static std::vector<int16_t> read_response(axis_stream_t &out,
                                          const std::vector<uint16_t>& tx_ids,
                                          const std::vector<size_t>& counts,
                                          bool& framing_ok) {
    std::vector<int16_t> got;
    framing_ok = (tx_ids.size() == counts.size());
    for (size_t response = 0; response < tx_ids.size(); ++response) {
        if (out.empty()) {
            framing_ok = false;
            break;
        }
        axis_word header = out.read();
        framing_ok &= header.last && header.data == tx_ids[response];
        for (size_t i = 0; i < counts[response]; ++i) {
            if (out.empty()) {
                framing_ok = false;
                break;
            }
            axis_word word = out.read();
            framing_ok &= ((bool)word.last == (i + 1 == counts[response]));
            got.push_back((int16_t)word.data.range(15, 0));
        }
    }
    framing_ok &= out.empty();
    return got;
}

static void send_cfg(axis_stream_t &in, uint16_t tx, uint8_t taps, const std::vector<int16_t>& coeffs) {
    in.write(make_axis(make_cmd(CMD_CONFIG, tx, taps), false));
    for (size_t i = 0; i < 8; ++i) {
        in.write(make_axis(make_coeff((i < coeffs.size()) ? coeffs[i] : 0), (i == 7) ? true : false));
    }
}

static void send_data(axis_stream_t &in, uint16_t tx, const std::vector<int16_t>& samples) {
    in.write(make_axis(make_cmd(CMD_DATA, tx, 0), false));
    in.write(make_axis(make_count(samples.size()), true));
    for (size_t i = 0; i < samples.size(); ++i) {
        in.write(make_axis(make_sample(samples[i]), (i + 1 == samples.size()) ? true : false));
    }
}

static void end_activation(axis_stream_t &in) {
    in.write(make_axis(make_cmd(CMD_END, 0, 0), true));
}

static bool run_valid_case(const std::vector<int16_t>& coeffs, const std::vector<int16_t>& samples,
                          const std::vector<int16_t>& exp, const char* label,
                          uint16_t tx_cfg, uint16_t tx_data) {
    axis_stream_t in("in_stream");
    axis_stream_t out("out_stream");
    ap_uint<32> halted = 0, error = 0, txid = 0, cycles = 0;

    send_cfg(in, tx_cfg, coeffs.size(), coeffs);
    send_data(in, tx_data, samples);
    end_activation(in);

    fir_accel(in, out, halted, error, txid, cycles);
    bool framing_ok = false;
    std::vector<int16_t> got = read_response(out, {tx_data}, {exp.size()}, framing_ok);
    bool ok = check_vec(got, exp, label);
    fprintf(stdout, "status_halted=%u error=%u txid=%u cycles=%u (%s)\n",
            halted.to_uint(), error.to_uint(), txid.to_uint(), cycles.to_uint(), label);
    return ok && framing_ok && (halted == 0) && (error == ERR_NO_ERROR) && (txid == 0);
}

static bool run_error_case(const std::vector<axis_word>& words, int expected_error,
                           uint16_t expected_tx, const char* label) {
    axis_stream_t in("in_stream");
    axis_stream_t out("out_stream");
    ap_uint<32> halted = 0, error = 0, txid = 0, cycles = 0;

    for (auto w : words) in.write(w);
    fir_accel(in, out, halted, error, txid, cycles);

    bool unread = !in.empty();
    bool out_empty = out.empty();
    bool status_ok = (halted == 1) && (error == expected_error) && (txid == expected_tx);
    fprintf(stdout, "CASE %s: halted=%u error=%u txid=%u unread=%d out_empty=%d cycles=%u\n",
            label, halted.to_uint(), error.to_uint(), txid.to_uint(), unread ? 1 : 0, out_empty ? 1 : 0, cycles.to_uint());
    return status_ok && unread && out_empty;
}

static bool run_recovery_case(const std::vector<int16_t>& coeffs, const std::vector<int16_t>& samples,
                             const std::vector<int16_t>& exp, const char* label,
                             uint16_t tx_cfg, uint16_t tx_data) {
    axis_stream_t in("in_stream");
    axis_stream_t out("out_stream");
    ap_uint<32> halted = 0, error = 0, txid = 0, cycles = 0;

    send_cfg(in, tx_cfg, coeffs.size(), coeffs);
    send_data(in, tx_data, samples);
    end_activation(in);

    fir_accel(in, out, halted, error, txid, cycles);
    bool framing_ok = false;
    std::vector<int16_t> got = read_response(out, {tx_data}, {exp.size()}, framing_ok);
    bool ok = (halted == 0) && (error == ERR_NO_ERROR) && (txid == 0) && check_vec(got, exp, label);
    fprintf(stdout, "recovery_status_halted=%u error=%u txid=%u cycles=%u (%s)\n",
            halted.to_uint(), error.to_uint(), txid.to_uint(), cycles.to_uint(), label);
    return ok && framing_ok;
}

int main() {
    bool ok = true;

    ok &= run_valid_case({16384, 16384}, {16384, 8192, 0}, {8192, 12288, 4096}, "fir_example_1", 1, 2);
    ok &= run_valid_case({16384, 16384}, {8192, 0}, {4096, 4096}, "fir_example_3", 3, 4);
    ok &= run_valid_case({16384}, {1, 3, -1, -3}, {0, 2, 0, -2}, "fir_rounding_halfway", 5, 6);
    ok &= run_valid_case({-32768, -32768}, {-32768, -32768}, {32767, 32767}, "fir_saturation", 7, 8);

    std::vector<int16_t> neg_coeffs = {32767, 32767};
    std::vector<int16_t> neg_samples = {-32768, -32768};
    std::vector<int16_t> neg_exp = {-32767, -32768};
    ok &= run_valid_case(neg_coeffs, neg_samples, neg_exp, "fir_negative_saturation", 9, 10);
    ok &= run_valid_case({32767}, {-32768}, {-32767}, "fir_negative_boundary_just_above", 11, 12);
    ok &= run_valid_case({-32768}, {-32768}, {32767}, "fir_positive_boundary", 13, 14);

    std::vector<int16_t> t8_coeffs(8, 16384);
    std::vector<int16_t> t8_samples(256, 16384);
    std::vector<int16_t> t8_exp = fir_reference(t8_coeffs, t8_samples);
    ok &= run_valid_case(t8_coeffs, t8_samples, t8_exp, "fir_t8_256", 15, 16);

    std::vector<int16_t> carry_coeffs = {16384, 16384};
    std::vector<int16_t> carry_data1 = {16384};
    std::vector<int16_t> carry_data2 = {8192, 0};
    std::vector<int16_t> carry_exp = {8192, 12288, 4096};
    axis_stream_t in1("carry_in");
    axis_stream_t out1("carry_out");
    ap_uint<32> halted = 0, error = 0, txid = 0, cycles = 0;
    send_cfg(in1, 0x20, 2, carry_coeffs);
    send_data(in1, 0x21, carry_data1);
    send_data(in1, 0x22, carry_data2);
    end_activation(in1);
    fir_accel(in1, out1, halted, error, txid, cycles);
    bool carry_framing_ok = false;
    std::vector<int16_t> carry_got = read_response(out1, {0x21, 0x22}, {1, 2}, carry_framing_ok);
    ok &= check_vec(carry_got, carry_exp, "fir_state_carry") && carry_framing_ok &&
          (halted == 0) && (error == ERR_NO_ERROR);

    axis_stream_t in_reconfig("reconfig_in");
    axis_stream_t out_reconfig("reconfig_out");
    ap_uint<32> reconfig_halted = 0, reconfig_error = 0, reconfig_txid = 0, reconfig_cycles = 0;
    send_cfg(in_reconfig, 0x30, 2, carry_coeffs);
    send_data(in_reconfig, 0x31, {16384, 8192, 0});
    send_cfg(in_reconfig, 0x32, 2, carry_coeffs);
    send_data(in_reconfig, 0x33, {8192, 0});
    end_activation(in_reconfig);
    fir_accel(in_reconfig, out_reconfig, reconfig_halted, reconfig_error, reconfig_txid, reconfig_cycles);
    bool reconfig_framing_ok = false;
    std::vector<int16_t> reconfig_got =
        read_response(out_reconfig, {0x31, 0x33}, {3, 2}, reconfig_framing_ok);
    const std::vector<int16_t> reconfig_exp = {8192, 12288, 4096, 4096, 4096};
    ok &= check_vec(reconfig_got, reconfig_exp, "fir_reconfiguration_resets_history") &&
          reconfig_framing_ok && (reconfig_halted == 0) &&
          (reconfig_error == ERR_NO_ERROR) && (reconfig_txid == 0);

    ok &= run_error_case({make_axis(make_cmd(0xFF, 0x12, 0), true), make_axis(make_cmd(CMD_END, 0, 0), true)}, ERR_UNKNOWN_COMMAND, 0x12, "err_unknown");
    ok &= run_recovery_case({16384, 16384}, {8192, 0}, {4096, 4096}, "recovery_after_unknown", 0xAA, 0xBB);

    ok &= run_error_case({
        make_axis(make_cmd(0xFF, 0xD1, 0), false),
        make_axis(make_cmd(CMD_END, 0, 0), true)
    }, ERR_MALFORMED_HEADER, 0xD1, "precedence_malformed_over_unknown");
    ok &= run_recovery_case({16384}, {1, 3}, {0, 2},
                            "recovery_after_precedence_malformed_unknown", 0xD3, 0xD4);

    ok &= run_error_case({
        make_axis(make_cmd(CMD_CONFIG, 0xD5, 9), true),
        make_axis(make_coeff(16384), true)
    }, ERR_MALFORMED_HEADER, 0xD5, "precedence_malformed_over_invalid_tap");
    ok &= run_recovery_case({16384}, {1, 3}, {0, 2},
                            "recovery_after_precedence_malformed_tap", 0xD6, 0xD7);

    ok &= run_error_case({
        make_axis(make_cmd(CMD_DATA, 0xD8, 0), false),
        make_axis(make_count(0), true),
        make_axis(make_sample(1), true)
    }, ERR_DATA_BEFORE_CONFIG, 0xD8, "precedence_data_before_config_over_invalid_count");
    ok &= run_recovery_case({16384}, {1, 3}, {0, 2},
                            "recovery_after_precedence_data_config", 0xD9, 0xDA);

    ok &= run_error_case({
        make_axis(make_cmd(CMD_CONFIG, 0x34, 9), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_cmd(CMD_END, 0, 0), true),
        make_axis(make_sample(0), false)
    }, ERR_INVALID_TAP_COUNT, 0x34, "err_invalid_tap");
    ok &= run_recovery_case({16384}, {1, 3}, {0, 2}, "recovery_after_tap_error", 0xCC, 0xDD);

    ok &= run_error_case({
        make_axis(make_cmd(CMD_DATA, 0x56, 0), false),
        make_axis(make_count(3), true),
        make_axis(make_sample(1), false),
        make_axis(make_sample(2), false),
        make_axis(make_sample(3), false),
        make_axis(make_cmd(CMD_END, 0, 0), true)
    }, ERR_DATA_BEFORE_CONFIG, 0x56, "err_data_before_config");
    ok &= run_recovery_case({16384}, {1, 3, -1}, {0, 2, 0}, "recovery_after_data_error", 0xEE, 0xFF);

    ok &= run_error_case({
        make_axis(make_cmd(CMD_CONFIG, 0x78, 2), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), true),
        make_axis(make_cmd(CMD_DATA, 0x78, 0), false),
        make_axis(make_count(0), true),
        make_axis(make_sample(1), true),
        make_axis(make_sample(2), false)
    }, ERR_INVALID_SAMPLE_COUNT, 0x78, "err_invalid_sample_count");
    ok &= run_recovery_case({16384, 16384}, {16384, 8192}, {8192, 12288}, "recovery_after_sample_error", 0x11, 0x22);

    ok &= run_error_case({
        make_axis(make_cmd(CMD_CONFIG, 0x79, 1), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), false),
        make_axis(make_coeff(16384), true),
        make_axis(make_cmd(CMD_DATA, 0x79, 0), false),
        make_axis(make_count(0x10000001), true),
        make_axis(make_sample(1), true)
    }, ERR_INVALID_SAMPLE_COUNT, 0x79, "err_invalid_sample_count_high_bits");
    ok &= run_recovery_case({16384}, {1}, {0}, "recovery_after_sample_high_bits", 0x23, 0x24);

    axis_stream_t in_malformed("bad_header_in");
    axis_stream_t out_malformed("bad_header_out");
    ap_uint<32> halted_bad = 0, err_bad = 0, tx_bad = 0, cyc_bad = 0;
    in_malformed.write(make_axis(make_cmd(CMD_CONFIG, 0x99, 2), false));
    in_malformed.write(make_axis(make_coeff(16384), false));
    in_malformed.write(make_axis(make_cmd(CMD_END, 0, 0), true));
    in_malformed.write(make_axis(make_sample(0), false));
    fir_accel(in_malformed, out_malformed, halted_bad, err_bad, tx_bad, cyc_bad);
    bool malformed_unread = !in_malformed.empty();
    bool malformed_output_empty = out_malformed.empty();
    ok &= (halted_bad == 1) && (err_bad == ERR_MALFORMED_HEADER) && (tx_bad == 0x99) &&
          malformed_unread && malformed_output_empty;
    fprintf(stdout, "CASE err_malformed_header: halted=%u error=%u txid=%u unread=%d out_empty=%d\n",
            halted_bad.to_uint(), err_bad.to_uint(), tx_bad.to_uint(), malformed_unread ? 1 : 0, malformed_output_empty ? 1 : 0);
    ok &= run_recovery_case({16384}, {1, 3}, {0, 2}, "recovery_after_malformed_header", 0xC1, 0xC2);

    axis_stream_t in_early("early_tlast_in");
    axis_stream_t out_early("early_tlast_out");
    ap_uint<32> halted_early = 0, err_early = 0, tx_early = 0, cyc_early = 0;
    send_cfg(in_early, 0xA1, 2, {16384, 16384});
    in_early.write(make_axis(make_cmd(CMD_DATA, 0xA2, 0), false));
    in_early.write(make_axis(make_count(2), true));
    in_early.write(make_axis(make_sample(16384), true));
    in_early.write(make_axis(make_sample(8192), false));
    in_early.write(make_axis(make_cmd(CMD_END, 0, 0), true));
    fir_accel(in_early, out_early, halted_early, err_early, tx_early, cyc_early);
    bool early_unread = !in_early.empty();
    bool early_output_empty = out_early.empty();
    ok &= (halted_early == 1) && (err_early == ERR_EARLY_TLAST) && (tx_early == 0xA2) &&
          early_unread && early_output_empty;
    fprintf(stdout, "CASE err_early_tlast: halted=%u error=%u txid=%u unread=%d out_empty=%d\n",
            halted_early.to_uint(), err_early.to_uint(), tx_early.to_uint(), early_unread ? 1 : 0, early_output_empty ? 1 : 0);
    ok &= run_recovery_case({16384}, {1, 3}, {0, 2}, "recovery_after_early_tlast", 0xC3, 0xC4);

    axis_stream_t in_missing("missing_tlast_in");
    axis_stream_t out_missing("missing_tlast_out");
    ap_uint<32> halted_missing = 0, err_missing = 0, tx_missing = 0, cyc_missing = 0;
    send_cfg(in_missing, 0xB1, 2, {16384, 16384});
    in_missing.write(make_axis(make_cmd(CMD_DATA, 0xB2, 0), false));
    in_missing.write(make_axis(make_count(2), true));
    in_missing.write(make_axis(make_sample(16384), false));
    in_missing.write(make_axis(make_sample(8192), false));
    in_missing.write(make_axis(make_cmd(CMD_END, 0, 0), true));
    fir_accel(in_missing, out_missing, halted_missing, err_missing, tx_missing, cyc_missing);
    bool missing_unread = !in_missing.empty();
    bool missing_output_empty = out_missing.empty();
    ok &= (halted_missing == 1) && (err_missing == ERR_MISSING_TLAST) && (tx_missing == 0xB2) &&
          missing_unread && missing_output_empty;
    fprintf(stdout, "CASE err_missing_tlast: halted=%u error=%u txid=%u unread=%d out_empty=%d\n",
            halted_missing.to_uint(), err_missing.to_uint(), tx_missing.to_uint(), missing_unread ? 1 : 0, missing_output_empty ? 1 : 0);
    ok &= run_recovery_case({16384}, {1, 3}, {0, 2}, "recovery_after_missing_tlast", 0xC5, 0xC6);

    return ok ? 0 : 1;
}
