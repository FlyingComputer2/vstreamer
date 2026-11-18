#ifndef VSTREAMER_CORE_CONFIG_KEYS_HPP
#define VSTREAMER_CORE_CONFIG_KEYS_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/component.hpp"

/*
 * Keys accepted by the typed component::configure(uint64_t key, uint64_t value).
 *
 * These are the keys a controller changes while streaming, so callers can set them without
 * formatting and parsing numbers. Each key is also accepted in string form under `name`; both
 * forms reach the same validating setter in the component. Keys that are only ever set in
 * string form (sizes, modes, paths, pre-open settings) stay inside their components.
 *
 *   bitrate         encoder target bitrate in kbit/s (encoders and encoding sources)
 *   gop             encoder GOP length in frames
 *   idr             request an IDR on the next frame; the value is ignored
 *
 * FEC is not a vstreamer key: the link runs it (winject-l3, per upstream).
 */
namespace vstreamer::keys
{

struct key
{
    std::string_view name;
    uint64_t         id;

    constexpr explicit key(std::string_view n) noexcept : name(n), id(query_key(n)) {}
};

inline constexpr key bitrate {"bitrate"};
inline constexpr key gop {"gop"};
inline constexpr key idr {"idr"};

inline constexpr key all[] = {bitrate, gop, idr};

namespace detail
{
constexpr bool ids_unique() noexcept
{
    constexpr size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; ++i)
    {
        for (size_t j = i + 1; j < n; ++j)
        {
            if (all[i].id == all[j].id)
            {
                return false;
            }
        }
    }
    return true;
}
}  // namespace detail

static_assert(detail::ids_unique(), "typed config key ids collide");

}  // namespace vstreamer::keys

#endif  // VSTREAMER_CORE_CONFIG_KEYS_HPP
