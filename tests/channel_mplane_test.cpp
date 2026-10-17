#include "apps/channel_sim/channel_mplane.hpp"
#include "apps/common/link_emulator.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

std::string ask(vstreamer::apps::link_emulator &link, const char *line)
{
    std::string reply;
    EXPECT_TRUE(vstreamer::apps::handle_channel_mplane_line(link, line, reply)) << line;
    return reply;
}

}  // namespace

TEST(ChannelMplaneTest, SetAndGet)
{
    vstreamer::apps::link_emulator link;
    EXPECT_EQ("ok\n", ask(link, "set link loss 2.5"));
    EXPECT_EQ("ok 2.5\n", ask(link, "get link loss"));
    EXPECT_EQ("ok\n", ask(link, "set link ge_p_gb 1"));
    EXPECT_EQ("ok\n", ask(link, "set link ge_p_bg 20"));
    EXPECT_EQ("ok\n", ask(link, "set link ge_loss_bad 80"));
    EXPECT_EQ("ok\n", ask(link, "set link max_kbps 12000"));
    EXPECT_EQ("ok\n", ask(link, "set link queue 0"));
    EXPECT_EQ("ok\n", ask(link, "set link seed 7"));
    EXPECT_EQ("ok loss=2.5 ge_p_gb=1 ge_p_bg=20 ge_loss_good=0 ge_loss_bad=80 max_kbps=12000 "
              "drop_dt_ms=1000 queue=0\n",
              ask(link, "get link config"));
    const vstreamer::apps::gilbert_elliott ge = link.gilbert_elliott_params();
    EXPECT_DOUBLE_EQ(1., ge.p_gb);
    EXPECT_DOUBLE_EQ(20., ge.p_bg);
    EXPECT_DOUBLE_EQ(80., ge.loss_bad);
}

TEST(ChannelMplaneTest, Stats)
{
    vstreamer::apps::link_emulator link;
    const std::string s = ask(link, "get link stats");
    EXPECT_EQ(0U, s.rfind("ok fwd_in=0 fwd_out=0 ", 0)) << s;
    EXPECT_NE(std::string::npos, s.find(" fwd_drop_burst=0 fwd_bursts=0 ")) << s;
    EXPECT_NE(std::string::npos, s.find(" rev_in=0 ")) << s;
    EXPECT_EQ("ok\n", ask(link, "set link stats reset"));
    EXPECT_EQ("err -22 EINVAL\n", ask(link, "set link stats 0"));
}

TEST(ChannelMplaneTest, Errors)
{
    vstreamer::apps::link_emulator link;
    EXPECT_EQ("err -22 EINVAL\n", ask(link, "set link loss 101"));
    EXPECT_EQ("err -22 EINVAL\n", ask(link, "set link loss x"));
    EXPECT_EQ("err -22 EINVAL\n", ask(link, "set link drop_dt_ms 0"));
    EXPECT_EQ("err -95 ENOTSUP\n", ask(link, "get link nope"));
    EXPECT_EQ("err -95 ENOTSUP\n", ask(link, "set link config 1"));
    EXPECT_EQ("err -2 ENOENT\n", ask(link, "get chan loss"));
    EXPECT_EQ("err usage: set link <key> <value>\n", ask(link, "set link loss"));
    EXPECT_EQ("err usage: get link <key>\n", ask(link, "get link loss 1"));

    std::string reply;
    EXPECT_FALSE(vstreamer::apps::handle_channel_mplane_line(link, "ping", reply));
}
