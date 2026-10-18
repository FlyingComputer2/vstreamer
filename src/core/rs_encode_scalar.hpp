#ifndef VSTREAMER_CORE_RS_ENCODE_SCALAR_HPP
#define VSTREAMER_CORE_RS_ENCODE_SCALAR_HPP

#include <cstdint>

namespace vstreamer
{

/*
 * Reed-Solomon parity encode with ISA-L's ec_encode_data() signature, for targets without an
 * ISA-L SIMD kernel (ESP32-P4). g_tbls comes from ec_init_tables(): one 32-byte table per
 * (parity row, source), bytes 0..15 = c * i and 16..31 = c * (i << 4).
 *
 * On the ESP32-P4 with CONFIG_VSTREAMER_RS_PIE (default on) this runs a PIE SIMD kernel, about
 * 2.8x faster than the table path; otherwise, or if rs_encode_selftest() found a mismatch, it
 * runs rs_encode_scalar_table(). Note that FreeRTOS pins a task to its core the first time it
 * uses PIE, so an unpinned thread that encodes FEC ends up pinned.
 */
void rs_encode_scalar(int len, int k, int rows, const unsigned char *g_tbls,
                      unsigned char *const *data, unsigned char *const *coding);

/* The portable table-lookup path of rs_encode_scalar(); on the ESP32-P4 rs_encode_scalar()
 * runs a PIE SIMD kernel instead and this stays as its reference and fallback. */
void rs_encode_scalar_table(int len, int k, int rows, const unsigned char *g_tbls,
                            unsigned char *const *data, unsigned char *const *coding);

/* "pie" or "table": the path rs_encode_scalar() currently takes. */
const char *rs_encode_scalar_impl();

/* Encodes random data both ways at misaligned addresses; returns the number of parity rows that
 * differ (0 = identical) and the mean microseconds per call of rs_encode_scalar() and of the
 * table path. now_us supplies a microsecond clock. The shards live in arena when given
 * ((k + 2 * rows) * (len + 8) bytes, e.g. a memory region under test), else on the heap.
 * Any mismatch turns the PIE kernel off for the rest of the run. */
int rs_encode_selftest(int k, int rows, int len, int iterations, int64_t (*now_us)(), int64_t *us_fast,
                       int64_t *us_table, uint8_t *arena = nullptr);

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_RS_ENCODE_SCALAR_HPP
