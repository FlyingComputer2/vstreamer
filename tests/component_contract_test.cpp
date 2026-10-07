#include "core/component.hpp"
#include "core/component_coder.hpp"
#include "core/component_factory.hpp"
#include "core/component_sink.hpp"
#include "core/component_source.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_type.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>

namespace
{

constexpr const char k_unknown_key[] = "no_such_key_xyz";

void expect_unknown_key_enotsup(vstreamer::component &c, const std::string &label)
{
    EXPECT_EQ(-ENOTSUP, c.configure(k_unknown_key, "1")) << label << " configure";
    std::string val;
    EXPECT_EQ(-ENOTSUP, c.query(k_unknown_key, &val)) << label << " query";
}

void expect_port_caps_parseable(vstreamer::component &c, const std::string &label)
{
    for (const bool is_input : {true, false})
    {
        const std::string size_key = is_input ? vstreamer::inport_size_key() : vstreamer::outport_size_key();
        std::string       size_val;
        if (0 != c.query(size_key, &size_val))
        {
            continue;
        }
        const int port_count = std::atoi(size_val.c_str());
        EXPECT_GE(port_count, 0) << label << ' ' << size_key;
        for (int port = 0; port < port_count; ++port)
        {
            const std::string caps_size_key =
                is_input ? vstreamer::inport_caps_size_key(static_cast<uint8_t>(port))
                         : vstreamer::outport_caps_size_key(static_cast<uint8_t>(port));
            std::string caps_size_val;
            EXPECT_EQ(0, c.query(caps_size_key, &caps_size_val)) << label << ' ' << caps_size_key;
            const int caps_count = std::atoi(caps_size_val.c_str());
            EXPECT_GE(caps_count, 0) << label << ' ' << caps_size_key;
            for (int entry = 0; entry < caps_count; ++entry)
            {
                const std::string type_key =
                    is_input ? vstreamer::inport_caps_field_key(static_cast<uint8_t>(port),
                                                                static_cast<size_t>(entry), "sdu_type")
                             : vstreamer::outport_caps_field_key(static_cast<uint8_t>(port),
                                                                 static_cast<size_t>(entry), "sdu_type");
                std::string type_val;
                EXPECT_EQ(0, c.query(type_key, &type_val)) << label << ' ' << type_key;
                vstreamer::sdu_type_e parsed = vstreamer::sdu_type_e::UNKNOWN;
                EXPECT_TRUE(vstreamer::parse_sdu_type(type_val, &parsed))
                    << label << ' ' << type_key << '=' << type_val;
                EXPECT_NE(vstreamer::sdu_type_e::UNKNOWN, parsed) << label << ' ' << type_val;
            }
        }
    }
}

}  // namespace

/* Unknown keys return -ENOTSUP (bad values return -EINVAL) on every component. */
TEST(ComponentContractTest, UnknownKeyIsEnotsup)
{
    int checked = 0;
    for (const char *name : {"noise", "v4l2", "stream_receiver"})
    {
        if (auto c = vstreamer::component_factory::create_source(name))
        {
            expect_unknown_key_enotsup(*c, name);
            ++checked;
        }
    }
    for (const char *name : {"jpeg_decoder_multicore", "h264_decoder_mpp", "h264_encoder_mpp",
                             "h264_encoder_cedar", "h264_encoder_intel", "rtp_h264_pay",
                             "rtp_h264_depay"})
    {
        if (auto c = vstreamer::component_factory::create_coder(name))
        {
            expect_unknown_key_enotsup(*c, name);
            ++checked;
        }
    }
    for (const char *name : {"mkv_sink", "stream_sender", "sdl_sink"})
    {
        if (auto c = vstreamer::component_factory::create_sink(name))
        {
            expect_unknown_key_enotsup(*c, name);
            ++checked;
        }
    }
    EXPECT_GT(checked, 0);
}

TEST(ComponentContractTest, PortCapsKeysParseable)
{
    int checked = 0;
    for (const char *name : {"noise", "v4l2", "stream_receiver"})
    {
        if (auto c = vstreamer::component_factory::create_source(name))
        {
            expect_port_caps_parseable(*c, name);
            ++checked;
        }
    }
    for (const char *name : {"jpeg_decoder_multicore", "h264_decoder_mpp", "h264_encoder_mpp",
                             "h264_encoder_cedar", "h264_encoder_intel", "rtp_h264_pay",
                             "rtp_h264_depay"})
    {
        if (auto c = vstreamer::component_factory::create_coder(name))
        {
            expect_port_caps_parseable(*c, name);
            ++checked;
        }
    }
    for (const char *name : {"mkv_sink", "stream_sender", "sdl_sink"})
    {
        if (auto c = vstreamer::component_factory::create_sink(name))
        {
            expect_port_caps_parseable(*c, name);
            ++checked;
        }
    }
    EXPECT_GT(checked, 0);
}
