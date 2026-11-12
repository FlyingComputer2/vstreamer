#include "components/h264_encoder_intel.hpp"
#include "core/config_keys.hpp"

#include <bfc/shared_sized_buffer.hpp>

#include <gtest/gtest.h>


#include <cerrno>
#include <string>

namespace
{

int cfg(vstreamer::component &c, const char *key, const char *val)
{
    return c.configure(key, val);
}

int qry(const vstreamer::component &c, const char *key, std::string *out)
{
    return c.query(key, out);
}

}  // namespace

TEST(H264EncoderIntelTest, ConfigureQueryNoHardware)
{
    vstreamer::h264_encoder_intel enc;

    EXPECT_EQ(cfg(enc, "rc", "cbr"), 0);
    std::string rc;
    EXPECT_EQ(qry(enc, "rc", &rc), 0);
    EXPECT_EQ(rc, "cbr");

    EXPECT_EQ(cfg(enc, "rc", "cqp"), 0);
    EXPECT_EQ(qry(enc, "rc", &rc), 0);
    EXPECT_EQ(rc, "cqp");

    EXPECT_EQ(cfg(enc, "rc", "fixqp"), 0);
    EXPECT_EQ(qry(enc, "rc", &rc), 0);
    EXPECT_EQ(rc, "cqp");

    EXPECT_EQ(cfg(enc, "bitrate", "5000"), 0);
    EXPECT_EQ(enc.configure(vstreamer::keys::bitrate.id, 3000), 0);
    std::string kbps;
    EXPECT_EQ(qry(enc, "bitrate", &kbps), 0);
    EXPECT_EQ(kbps, "3000");
    EXPECT_EQ(cfg(enc, "cbr", "5000000"), -ENOTSUP);
    EXPECT_EQ(cfg(enc, "bps", "5000000"), -ENOTSUP);

    EXPECT_EQ(cfg(enc, "low_power", "auto"), 0);
    EXPECT_EQ(cfg(enc, "low_power", "0"), 0);
    EXPECT_EQ(cfg(enc, "low_power", "1"), 0);
    EXPECT_EQ(cfg(enc, "low_power", "2"), -EINVAL);

    EXPECT_EQ(cfg(enc, "vbv_ms", "500"), 0);
    EXPECT_EQ(cfg(enc, "vbv_ms", "0"), -EINVAL);
    EXPECT_EQ(cfg(enc, "vbv_ms", "2001"), -EINVAL);

    EXPECT_EQ(cfg(enc, "bitrate", "-1"), -EINVAL);
    EXPECT_EQ(cfg(enc, "bitrate", "200001"), -EINVAL);
    EXPECT_EQ(enc.configure(vstreamer::keys::bitrate.id, 200001), -EINVAL);
    EXPECT_EQ(enc.configure(vstreamer::keys::gop.id, 0), -EINVAL);
    EXPECT_EQ(enc.configure(vstreamer::keys::gop.id, 60), 0);
    std::string gop;
    EXPECT_EQ(qry(enc, "gop", &gop), 0);
    EXPECT_EQ(gop, "60");
    EXPECT_EQ(enc.configure(vstreamer::keys::idr.id, 0), 0);
    EXPECT_EQ(enc.configure(vstreamer::keys::fec_k.id, 4), -ENOTSUP);
}
