#include "core/thread_spawn.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "core/component_factory.hpp"
#include "core/component_sink.hpp"

using vstreamer::thread_place;
using vstreamer::thread_place_configure;
using vstreamer::thread_place_query;

TEST(ThreadPlaceTest, DefaultsToAnyCoreAndDefaultPrio)
{
    thread_place p;
    const auto o = p.opts("t", 1024);
    EXPECT_EQ(o.cpu, -1);
    EXPECT_EQ(o.prio, -1);
    EXPECT_STREQ(o.name, "t");
    EXPECT_EQ(o.stack_bytes, 1024u);
}

TEST(ThreadPlaceTest, ConfigureAndQueryRoleKeys)
{
    thread_place p;
    EXPECT_EQ(thread_place_configure(&p, "send", "send_cpu", "1", false), 0);
    EXPECT_EQ(thread_place_configure(&p, "send", "send_prio", "14", false), 0);
    std::string v;
    EXPECT_EQ(thread_place_query(p, "send", "send_cpu", &v), 0);
    EXPECT_EQ(v, "1");
    EXPECT_EQ(thread_place_query(p, "send", "send_prio", &v), 0);
    EXPECT_EQ(v, "14");
    EXPECT_EQ(p.opts("vs_send", 0).cpu, 1);
    EXPECT_EQ(p.opts("vs_send", 0).prio, 14);
    EXPECT_EQ(thread_place_configure(&p, "send", "send_cpu", "-1", false), 0);
    EXPECT_EQ(p.opts("vs_send", 0).cpu, -1);
}

TEST(ThreadPlaceTest, OtherKeysAreNotSupported)
{
    thread_place p;
    std::string v;
    for (const char *key :
         {"send", "send_", "send_cpux", "sendx_cpu", "recv_cpu", "cpu", "_cpu"})
    {
        EXPECT_EQ(thread_place_configure(&p, "send", key, "0", false), -ENOTSUP)
            << key;
        EXPECT_EQ(thread_place_query(p, "send", key, &v), -ENOTSUP) << key;
    }
}

TEST(ThreadPlaceTest, BusyWhileOpen)
{
    thread_place p;
    EXPECT_EQ(thread_place_configure(&p, "send", "send_cpu", "1", true), -EBUSY);
    EXPECT_EQ(thread_place_configure(&p, "send", "send_prio", "x", true), -EBUSY);
    EXPECT_EQ(thread_place_configure(&p, "send", "other", "1", true), -ENOTSUP);
    EXPECT_EQ(p.opts("t", 0).cpu, -1);
}

TEST(ThreadPlaceTest, RejectsOutOfRange)
{
    thread_place p;
    for (const char *value : {"-2", "x", "", "1024"})
    {
        EXPECT_EQ(thread_place_configure(&p, "send", "send_cpu", value, false),
                  -EINVAL)
            << value;
    }
    EXPECT_EQ(thread_place_configure(&p, "send", "send_prio", "100", false), -EINVAL);
    EXPECT_EQ(p.opts("t", 0).cpu, -1);
}

#if defined(__linux__)
TEST(ThreadSpawnTest, PinsNewThreadOnLinux)
{
    cpu_set_t parent;
    ASSERT_EQ(pthread_getaffinity_np(pthread_self(), sizeof(parent), &parent),
              0);
    int cpu = -1;
    for (int c = 0; c < CPU_SETSIZE; c++)
    {
        if (CPU_ISSET(c, &parent))
        {
            cpu = c;
            break;
        }
    }
    ASSERT_GE(cpu, 0);
    int seen_count = 0;
    bool seen_cpu = false;
    auto t = vstreamer::spawn_thread(
        vstreamer::thread_opts{"pin", 0, cpu},
        [&](int want)
        {
            cpu_set_t s;
            pthread_getaffinity_np(pthread_self(), sizeof(s), &s);
            seen_count = CPU_COUNT(&s);
            seen_cpu = CPU_ISSET(want, &s);
        },
        cpu);
    t.join();
    EXPECT_EQ(seen_count, 1);
    EXPECT_TRUE(seen_cpu);

    cpu_set_t after;
    ASSERT_EQ(pthread_getaffinity_np(pthread_self(), sizeof(after), &after), 0);
    EXPECT_TRUE(CPU_EQUAL(&parent, &after));
}
#endif

TEST(ThreadPlaceTest, StreamSenderExposesSendKeysOnly)
{
    auto snd = vstreamer::component_factory::create_sink("stream_sender");
    if (!snd)
    {
        GTEST_SKIP() << "stream_sender not built";
    }
    EXPECT_EQ(snd->configure("send_cpu", "0"), 0);
    EXPECT_EQ(snd->configure("telemetry_prio", "7"), -ENOTSUP);
    std::string v;
    EXPECT_EQ(snd->query("send_cpu", &v), 0);
    EXPECT_EQ(v, "0");
    EXPECT_EQ(snd->query("telemetry_prio", &v), -ENOTSUP);
    EXPECT_EQ(snd->query("send_prio", &v), 0);
    EXPECT_EQ(v, "-1");

    ASSERT_EQ(snd->configure("stream", "127.0.0.1:9"), 0);
    ASSERT_EQ(snd->configure("telemetry", "off"), 0);
    ASSERT_EQ(snd->open(), 0);
    EXPECT_EQ(snd->configure("send_cpu", "-1"), -EBUSY);
    EXPECT_EQ(snd->query("send_cpu", &v), 0);
    EXPECT_EQ(v, "0");
    snd->close();
    EXPECT_EQ(snd->configure("send_cpu", "-1"), 0);
}
