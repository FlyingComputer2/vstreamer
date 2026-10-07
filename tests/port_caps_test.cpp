#include "core/port_caps.hpp"

#include <gtest/gtest.h>

#include <cerrno>

namespace
{

vstreamer::port_caps_field field(std::string name, std::string value)
{
    vstreamer::port_caps_field f;
    f.name = std::move(name);
    EXPECT_TRUE(vstreamer::parse_int32_constraint(value, &f.constraint));
    return f;
}

}  // namespace

TEST(PortCapsTest, ParseRangeListSingle)
{
    vstreamer::int32_constraint c;
    EXPECT_TRUE(vstreamer::parse_int32_constraint("64..4096", &c));
    EXPECT_TRUE(c.matches(100));
    EXPECT_FALSE(c.matches(50));

    EXPECT_TRUE(vstreamer::parse_int32_constraint("30,60", &c));
    EXPECT_TRUE(c.matches(30));
    EXPECT_TRUE(c.matches(60));
    EXPECT_FALSE(c.matches(45));

    EXPECT_TRUE(vstreamer::parse_int32_constraint("1920", &c));
    EXPECT_TRUE(c.matches(1920));
    EXPECT_FALSE(c.matches(1921));
}

TEST(PortCapsTest, ParseMalformed)
{
    vstreamer::int32_constraint c;
    EXPECT_FALSE(vstreamer::parse_int32_constraint("", &c));
    EXPECT_FALSE(vstreamer::parse_int32_constraint("a..b", &c));
    EXPECT_FALSE(vstreamer::parse_int32_constraint("10..5", &c));
    EXPECT_FALSE(vstreamer::parse_int32_constraint("1,,2", &c));
}

TEST(PortCapsTest, MatchVideoRawFields)
{
    vstreamer::port_caps_entry entry;
    entry.sdu_type = vstreamer::sdu_type_e::CAPS_VIDEO_RAW;
    entry.fields.push_back(field("width", "640..1920"));
    entry.fields.push_back(field("height", "480"));

    vstreamer::video_raw_caps ok {1280, 480, 1280, 480, 30, 1};
    EXPECT_TRUE(vstreamer::match(entry, ok));

    ok.width = 32;
    EXPECT_FALSE(vstreamer::match(entry, ok));
}

TEST(PortCapsTest, KeyBuildersAndQuery)
{
    vstreamer::port_desc p0;
    vstreamer::port_caps_entry e0;
    e0.sdu_type = vstreamer::sdu_type_e::NV12;
    e0.fields.push_back(field("width", "64..4096"));
    p0.caps.push_back(e0);
    vstreamer::port_caps_entry e1;
    e1.sdu_type = vstreamer::sdu_type_e::MJPEG;
    p0.caps.push_back(e1);

    vstreamer::port_desc p1;
    vstreamer::port_caps_entry e2;
    e2.sdu_type = vstreamer::sdu_type_e::H264_AU;
    p1.caps.push_back(e2);
    vstreamer::port_caps_entry e3;
    e3.sdu_type = vstreamer::sdu_type_e::RTP;
    p1.caps.push_back(e3);

    const std::vector<vstreamer::port_desc> ports {p0, p1};
    std::string out;

    EXPECT_EQ(0, vstreamer::port_caps_query(ports, true, "inport.size", &out));
    EXPECT_EQ("2", out);

    EXPECT_EQ(0, vstreamer::port_caps_query(ports, true, "inport-0.caps.size", &out));
    EXPECT_EQ("2", out);

    EXPECT_EQ(0, vstreamer::port_caps_query(ports, true, "inport-0.caps-0.sdu_type", &out));
    EXPECT_EQ("NV12", out);

    EXPECT_EQ(0, vstreamer::port_caps_query(ports, true, "inport-0.caps-0.width", &out));
    EXPECT_EQ("64..4096", out);

    EXPECT_EQ(0, vstreamer::port_caps_query(ports, true, "inport-0.caps-0.height", &out));
    EXPECT_EQ("", out);

    EXPECT_EQ(0, vstreamer::port_caps_query(ports, false, "outport-1.caps-1.sdu_type", &out));
    EXPECT_EQ("RTP", out);

    EXPECT_EQ(-EINVAL, vstreamer::port_caps_query(ports, true, "inport-9.caps.size", &out));
}
