#include "components/sdl_nv12_presenter.hpp"
#include "components/sdl_sink.hpp"
#include "core/component_factory.hpp"
#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"
#include "core/shared_sized_buffer.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace
{

vstreamer::component_pdu make_nv12_caps(int w, int h)
{
    vstreamer::video_raw_caps caps {};
    caps.width = w;
    caps.height = h;
    return vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_RAW, caps, 0, 0);
}

vstreamer::component_pdu make_nv12_pdu(int w, int h, uint64_t ts_us)
{
    const size_t y = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t sz = y + y / 2;
    std::vector<uint8_t> bytes(sz, 0x10);
    vstreamer::component_pdu pdu;
    pdu.ts_us = ts_us;
    pdu.sdu_type = vstreamer::sdu_type_e::NV12;
    pdu.port = 0;
    pdu.sdu = vstreamer::shared_sized_buffer::copy_from(bytes.data(), bytes.size());
    return pdu;
}

}  // namespace

TEST(SdlSinkTest, VideoDriverConfigureAndQuery)
{
    vstreamer::sdl_sink sink;
    std::string         val;
    EXPECT_EQ(0, sink.query("video_driver", &val));
    EXPECT_EQ("auto", val);
    EXPECT_EQ(0, sink.configure("video_driver", "kmsdrm"));
    EXPECT_EQ(0, sink.query("video_driver", &val));
    EXPECT_EQ("kmsdrm", val);
}

TEST(SdlSinkTest, FactoryKmsdrmAlias)
{
    auto c = vstreamer::component_factory::create_sink("sdl_kmsdrm");
    ASSERT_NE(nullptr, c);
    EXPECT_EQ("sdl_sink", c->name());
    std::string val;
    EXPECT_EQ(0, c->query("video_driver", &val));
    EXPECT_EQ("kmsdrm", val);
}

TEST(SdlSinkTest, PortCapsAdvertisesNv12)
{
    vstreamer::sdl_sink sink;
    std::string         val;
    ASSERT_EQ(0, sink.query("inport-0.caps-0.sdu_type", &val));
    EXPECT_EQ("CAPS_VIDEO_RAW", val);
    ASSERT_EQ(0, sink.query("inport-0.caps-1.sdu_type", &val));
    EXPECT_EQ("NV12", val);
}

TEST(SdlSinkTest, PduInputRequiresOpen)
{
    vstreamer::sdl_sink sink;
    ASSERT_EQ(-EBADF, sink.input(make_nv12_caps(64, 64)));
}

TEST(SdlNv12PresenterTest, TryEnqueueEagainWhenQueueFull)
{
    vstreamer::sdl_nv12_presenter present("sdl_sink_test", "dummy");
    present.set_queue_capacity(1);
    vstreamer::frame_data f;
    f.kind = vstreamer::media_kind_e::NV12;
    f.width = 4;
    f.height = 4;
    const size_t sz = 4 * 4 + 8;
    f.buf = vstreamer::shared_sized_buffer::copy_from(std::vector<uint8_t>(sz, 0).data(), sz);
    ASSERT_EQ(0, present.try_enqueue(f));
    ASSERT_EQ(-EAGAIN, present.try_enqueue(f));
}

TEST(SdlNv12PresenterTest, EnqueueDropOldestWhenQueueFull)
{
    vstreamer::sdl_nv12_presenter present("sdl_sink_test", "dummy");
    present.set_queue_capacity(1);
    vstreamer::frame_data f;
    f.kind = vstreamer::media_kind_e::NV12;
    f.width = 4;
    f.height = 4;
    const size_t sz = 4 * 4 + 8;
    f.buf = vstreamer::shared_sized_buffer::copy_from(std::vector<uint8_t>(sz, 0).data(), sz);
    bool dropped = false;
    ASSERT_EQ(0, present.enqueue_drop(f, &dropped));
    EXPECT_FALSE(dropped);
    ASSERT_EQ(0, present.enqueue_drop(f, &dropped));
    EXPECT_TRUE(dropped);
    EXPECT_EQ(1U, present.queue_size());
}
