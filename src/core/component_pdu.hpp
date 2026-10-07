#ifndef VSTREAMER_CORE_COMPONENT_PDU_HPP
#define VSTREAMER_CORE_COMPONENT_PDU_HPP

#include <cstdint>

#include "core/sdu_type.hpp"
#include "core/shared_sized_buffer.hpp"

namespace vstreamer
{

enum class pdu_flag_e : uint8_t
{
    KEY = 1,
    AU_END = 2,
    DISCONT = 4,
    EOS = 8,
};

/*
 * Bytes in sdu are immutable once the PDU has been passed to input() or returned
 * from output(). Code that needs to modify bytes must own the only reference
 * (sdu unique ownership) or copy.
 */
struct component_pdu
{
    uint64_t            ts_us = 0;
    uint64_t            seq = 0;
    sdu_type_e          sdu_type = sdu_type_e::UNKNOWN;
    uint8_t             port = 0;
    uint8_t             flags = 0;
    shared_sized_buffer sdu;
};

[[nodiscard]] inline bool has_flag(const component_pdu &pdu, pdu_flag_e flag)
{
    return (pdu.flags & static_cast<uint8_t>(flag)) != 0;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_PDU_HPP
