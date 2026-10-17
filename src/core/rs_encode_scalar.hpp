#ifndef VSTREAMER_CORE_RS_ENCODE_SCALAR_HPP
#define VSTREAMER_CORE_RS_ENCODE_SCALAR_HPP

namespace vstreamer
{

/*
 * Reed-Solomon parity encode with ISA-L's ec_encode_data() signature, for targets without an
 * ISA-L SIMD kernel (ESP32-P4). g_tbls comes from ec_init_tables(): one 32-byte table per
 * (parity row, source), bytes 0..15 = c * i and 16..31 = c * (i << 4).
 *
 * ISA-L's portable ec_encode_data_base() computes every product with two log lookups and a
 * modular add and walks all sources byte by byte; this walks one source at a time through its
 * nibble table (two lookups per byte, sequential access), several times faster.
 */
void rs_encode_scalar(int len, int k, int rows, const unsigned char *g_tbls,
                      unsigned char *const *data, unsigned char *const *coding);

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_RS_ENCODE_SCALAR_HPP
