#include "components/sdl_sink.hpp"
#include "core/component_factory.hpp"

#include <gtest/gtest.h>

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
