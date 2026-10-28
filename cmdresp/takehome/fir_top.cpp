#include <ap_int.h>
#include <ap_axi_sdata.h>
#include <hls_stream.h>
#include <stdint.h>

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

typedef ap_uint<32> word_t;
typedef ap_int<16> q15_t;
typedef ap_axis<32, 0, 0, 0> axis_word;

template <typename T>
static T saturate_q15(T x) {
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return x;
}

static int round_q15_even(long long acc) {
    long long q = acc / 32768LL;
    long long rem = acc % 32768LL;
    long long abs_rem = rem < 0 ? -rem : rem;
    if (abs_rem > 16384LL || (abs_rem == 16384LL && (q & 1LL))) {
        if (acc >= 0) ++q;
        else --q;
    }
    return (int)saturate_q15<int>((int)q);
}

static int16_t pack_result(int v) {
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void signal_error(ap_uint<32> &status_halted,
                        ap_uint<32> &status_error,
                        ap_uint<32> &status_txid,
                        ap_uint<16> tx_id,
                        ap_uint<32> code) {
    status_halted = 1;
    status_error = code;
    status_txid = tx_id;
}

void fir_accel(hls::stream<axis_word> &in, hls::stream<axis_word> &out,
               ap_uint<32> &status_halted,
               ap_uint<32> &status_error,
               ap_uint<32> &status_txid,
               ap_uint<32> &status_cycles) {
#pragma HLS INTERFACE axis port=in
#pragma HLS INTERFACE axis port=out
#pragma HLS INTERFACE s_axilite port=status_halted bundle=ctrl
#pragma HLS INTERFACE s_axilite port=status_error bundle=ctrl
#pragma HLS INTERFACE s_axilite port=status_txid bundle=ctrl
#pragma HLS INTERFACE s_axilite port=status_cycles bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return bundle=ctrl
#pragma HLS PIPELINE II=1

    status_halted = 0;
    status_error = ERR_NO_ERROR;
    status_txid = 0;
    status_cycles = 0;

    bool configured = false;
    int tap_count = 0;
    q15_t coeffs[MAX_TAPS];
    q15_t history[MAX_TAPS];
    for (int i = 0; i < MAX_TAPS; ++i) {
#pragma HLS UNROLL
        coeffs[i] = 0;
        history[i] = 0;
    }

    bool done = false;
    while (!done) {
        if (in.empty()) {
            break;
        }
        axis_word in_word = in.read();
        word_t h = in_word.data;
        ap_uint<8> cmd = h.range(7, 0);
        ap_uint<16> tx_id = h.range(23, 8);
        ap_uint<8> field = h.range(31, 24);

        if (cmd == CMD_CONFIG || cmd == CMD_DATA) {
            if (in_word.last) {
                signal_error(status_halted, status_error, status_txid, tx_id, ERR_MALFORMED_HEADER);
                done = true;
                continue;
            }
        } else if (cmd == CMD_END) {
            if (!in_word.last) {
                signal_error(status_halted, status_error, status_txid, tx_id, ERR_MALFORMED_HEADER);
                done = true;
                continue;
            }
        } else if (!in_word.last) {
            signal_error(status_halted, status_error, status_txid, tx_id, ERR_MALFORMED_HEADER);
            done = true;
            continue;
        }

        if (cmd == CMD_END) {
            done = true;
            continue;
        }

        if (cmd == CMD_CONFIG) {
            if (field < 1 || field > MAX_TAPS) {
                signal_error(status_halted, status_error, status_txid, tx_id, ERR_INVALID_TAP_COUNT);
                done = true;
                continue;
            }
            tap_count = field;
            for (int i = 0; i < MAX_TAPS; ++i) {
#pragma HLS UNROLL
                coeffs[i] = 0;
                history[i] = 0;
            }
            for (int i = 0; i < MAX_TAPS; ++i) {
                if (in.empty()) {
                    signal_error(status_halted, status_error, status_txid, tx_id, ERR_MALFORMED_HEADER);
                    done = true;
                    break;
                }
                axis_word coeff_word = in.read();
                if (i < (MAX_TAPS - 1) && coeff_word.last) {
                    signal_error(status_halted, status_error, status_txid, tx_id, ERR_MALFORMED_HEADER);
                    done = true;
                    break;
                }
                if (i == (MAX_TAPS - 1) && !coeff_word.last) {
                    signal_error(status_halted, status_error, status_txid, tx_id, ERR_MALFORMED_HEADER);
                    done = true;
                    break;
                }
                coeffs[i] = (q15_t)coeff_word.data.range(15, 0);
            }
            if (!done) {
                configured = true;
            }
            continue;
        }

        if (cmd == CMD_DATA) {
            if (!configured) {
                signal_error(status_halted, status_error, status_txid, tx_id, ERR_DATA_BEFORE_CONFIG);
                done = true;
                continue;
            }
            if (in.empty()) {
                signal_error(status_halted, status_error, status_txid, tx_id, ERR_MALFORMED_HEADER);
                done = true;
                continue;
            }
            axis_word count_word = in.read();
            ap_uint<32> sample_count = count_word.data;
            if (!count_word.last) {
                signal_error(status_halted, status_error, status_txid, tx_id, ERR_MALFORMED_HEADER);
                done = true;
                continue;
            }
            if (sample_count == 0 || sample_count > MAX_SAMPLES || sample_count.range(31, 9) != 0) {
                signal_error(status_halted, status_error, status_txid, tx_id, ERR_INVALID_SAMPLE_COUNT);
                done = true;
                continue;
            }

            int n = (int)sample_count.to_uint();
            q15_t sample_buf[MAX_SAMPLES];
            for (int i = 0; i < n; ++i) {
                if (in.empty()) {
                    signal_error(status_halted, status_error, status_txid, tx_id, ERR_MISSING_TLAST);
                    done = true;
                    break;
                }
                axis_word sample_word = in.read();
                if (i + 1 < n && sample_word.last) {
                    signal_error(status_halted, status_error, status_txid, tx_id, ERR_EARLY_TLAST);
                    done = true;
                    break;
                }
                if (i + 1 == n && !sample_word.last) {
                    signal_error(status_halted, status_error, status_txid, tx_id, ERR_MISSING_TLAST);
                    done = true;
                    break;
                }
                sample_buf[i] = (q15_t)sample_word.data.range(15, 0);
            }
            if (done) {
                continue;
            }

            axis_word resp_hdr;
            resp_hdr.data = (word_t)tx_id;
            resp_hdr.last = 1;
            resp_hdr.keep = 0xF;
            resp_hdr.strb = 0xF;
            out.write(resp_hdr);

            // status_cycles reports the accepted payload-word count for the current DATA burst.
            // It is latched after the loop and is not a hardware cycle counter with stall timestamps.
            ap_uint<32> accepted_cycles = 0;
            for (int i = 0; i < n; ++i) {
                for (int j = tap_count - 1; j > 0; --j) {
#pragma HLS UNROLL factor=8
                    history[j] = history[j - 1];
                }
                history[0] = sample_buf[i];
                long long acc = 0;
                for (int k = 0; k < tap_count; ++k) {
#pragma HLS UNROLL factor=8
                    acc += (long long)history[k] * (long long)coeffs[k];
                }
                int result = round_q15_even(acc);
                axis_word outw;
                outw.data = 0;
                outw.data.range(15, 0) = pack_result(result);
                outw.last = (i == n - 1) ? 1 : 0;
                outw.keep = 0xF;
                outw.strb = 0xF;
                out.write(outw);
                accepted_cycles = i + 1;
            }
            status_cycles = accepted_cycles;
            continue;
        }

        signal_error(status_halted, status_error, status_txid, tx_id, ERR_UNKNOWN_COMMAND);
        done = true;
    }
}
