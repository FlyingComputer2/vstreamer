#ifndef VSTREAMER_CORE_PORT_CAPS_HPP
#define VSTREAMER_CORE_PORT_CAPS_HPP

#include <cstdint>

#include <string>
#include <string_view>
#include <vector>

#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"

namespace vstreamer
{

struct int32_constraint
{
    bool     any = true;
    int32_t  single = 0;
    bool     is_range = false;
    int32_t  range_min = 0;
    int32_t  range_max = 0;
    std::vector<int32_t> list_values;

    [[nodiscard]] bool matches(int32_t v) const;
};

struct port_caps_field
{
    std::string      name;
    int32_constraint constraint;
};

struct port_caps_entry
{
    sdu_type_e                  sdu_type = sdu_type_e::UNKNOWN;
    std::vector<port_caps_field> fields;
};

struct port_desc
{
    std::vector<port_caps_entry> caps;
};

[[nodiscard]] std::string inport_size_key();
[[nodiscard]] std::string outport_size_key();
[[nodiscard]] std::string inport_caps_size_key(uint8_t port);
[[nodiscard]] std::string outport_caps_size_key(uint8_t port);
[[nodiscard]] std::string inport_caps_field_key(uint8_t port, size_t caps_index,
                                                std::string_view field);
[[nodiscard]] std::string outport_caps_field_key(uint8_t port, size_t caps_index,
                                                 std::string_view field);

/* Returns true on success; false if value is malformed. */
[[nodiscard]] bool parse_int32_constraint(std::string_view value, int32_constraint *out);

[[nodiscard]] bool match(const port_caps_entry &entry, const video_raw_caps &caps);
[[nodiscard]] bool match(const port_caps_entry &entry, const video_coded_caps &caps);
[[nodiscard]] bool match(const port_caps_entry &entry, const audio_caps &caps);
[[nodiscard]] bool match(const port_caps_entry &entry, const component_pdu &caps_pdu);

/*
 * Answers port-caps query keys for a fixed port table. 0 / -ENOTSUP / -EINVAL.
 */
[[nodiscard]] int port_caps_query(const std::vector<port_desc> &ports, bool is_input,
                                  std::string_view key, std::string *out);

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_PORT_CAPS_HPP
