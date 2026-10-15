#include "core/pipeline.hpp"
#include "core/pipeline_queue.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <climits>
#include <cstdint>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "components/components_config.hpp"
#include "core/component_coder.hpp"
#include "core/component_sink.hpp"
#include "core/component_source.hpp"
#include "core/sdu_caps.hpp"
#include "test_pdu_helpers.hpp"

namespace
{

using namespace std::chrono_literals;
using vstreamer::component_pdu;
using vstreamer::pipeline;
using vstreamer::pipeline_queue;
using vstreamer::pipeline_state_e;

/* Open/close calls of every fake, in order, shared by one test. */
struct call_log
{
    std::mutex               mu;
    std::vector<std::string> calls;

    void add(const std::string &s)
    {
        std::lock_guard<std::mutex> lock(mu);
        calls.push_back(s);
    }

    std::vector<std::string> get()
    {
        std::lock_guard<std::mutex> lock(mu);
        return calls;
    }
};

/* Emits `count` STREAM_DGRAM PDUs with seq 0..count-1. After that, output() returns
 * `idle_result` (default -EAGAIN). */
class fake_source : public vstreamer::component_source
{
public:
    fake_source(std::string tag, call_log *log, uint64_t count) : tag_(std::move(tag)), log_(log), count_(count) {}

    [[nodiscard]] std::string name() const override { return "fake_source"; }

    int open() override
    {
        log_->add("open " + tag_);
        return 0;
    }

    void close() override { log_->add("close " + tag_); }

    int output(component_pdu &out) override
    {
        output_calls.fetch_add(1);
        std::lock_guard<std::mutex> lock(mu_);
        if (!ready.load())
        {
            return -EAGAIN;
        }
        if (next_ >= count_)
        {
            return idle_result_;
        }
        out = vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(next_), 16);
        out.seq = next_++;
        return 0;
    }

    [[nodiscard]] int64_t next_deadline_ns() const override { return deadline_ns.load(); }

    std::atomic<int64_t> deadline_ns {INT64_MAX};
    /* While false, output() returns -EAGAIN even when PDUs remain (a node not ready yet). */
    std::atomic<bool>     ready {true};
    std::atomic<uint64_t> output_calls {0};

    void set_idle_result(int r)
    {
        std::lock_guard<std::mutex> lock(mu_);
        idle_result_ = r;
    }

    void add_more(uint64_t n)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            count_ += n;
        }
        notify_wakeup();
    }

    int configure(std::string_view key, std::string_view value) override
    {
        if ("label" == key)
        {
            std::lock_guard<std::mutex> lock(mu_);
            label_ = std::string(value);
            return 0;
        }
        return -ENOTSUP;
    }

    int query(std::string_view key, std::string *value) const override
    {
        if ("label" == key)
        {
            std::lock_guard<std::mutex> lock(mu_);
            *value = label_;
            return 0;
        }
        return -ENOTSUP;
    }

private:
    std::string        tag_;
    call_log          *log_;
    mutable std::mutex mu_;
    uint64_t           count_;
    uint64_t           next_ = 0;
    int                idle_result_ = -EAGAIN;
    std::string        label_;
};

/* Passes PDUs through. input() returns `fail_with` for the PDU with seq `fail_at`. */
class fake_coder : public vstreamer::component_coder
{
public:
    fake_coder(std::string tag, call_log *log) : tag_(std::move(tag)), log_(log) {}

    [[nodiscard]] std::string name() const override { return "fake_coder"; }

    int open() override
    {
        log_->add("open " + tag_);
        return 0;
    }

    void close() override { log_->add("close " + tag_); }

    int input(component_pdu &&in) override
    {
        if (in.seq == fail_at)
        {
            return fail_with;
        }
        q_.push_back(std::move(in));
        return 0;
    }

    int output(component_pdu &out) override
    {
        if (q_.empty())
        {
            return -EAGAIN;
        }
        out = std::move(q_.front());
        q_.pop_front();
        return 0;
    }

    int configure(std::string_view, std::string_view) override { return -ENOTSUP; }
    int query(std::string_view, std::string *) const override { return -ENOTSUP; }

    uint64_t fail_at = UINT64_MAX;
    int      fail_with = 0;

private:
    std::string               tag_;
    call_log                 *log_;
    std::deque<component_pdu> q_;
};

/* Records what it receives. Refuses with -EAGAIN while `blocked`; returns -EINVAL for odd seq
 * when `reject_odd`. */
class fake_sink : public vstreamer::component_sink
{
public:
    fake_sink(std::string tag, call_log *log) : tag_(std::move(tag)), log_(log) {}

    [[nodiscard]] std::string name() const override { return "fake_sink"; }

    int open() override
    {
        log_->add("open " + tag_);
        return open_result;
    }

    void close() override { log_->add("close " + tag_); }

    int input(component_pdu &&in) override
    {
        attempts.fetch_add(1);
        if (blocked.load())
        {
            return -EAGAIN;
        }
        if (reject_odd && (in.seq % 2U) == 1U)
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> lock(mu_);
        got_.push_back(in.seq);
        ports_.push_back(in.port);
        return 0;
    }

    int configure(std::string_view, std::string_view) override { return -ENOTSUP; }
    int query(std::string_view, std::string *) const override { return -ENOTSUP; }

    std::vector<uint64_t> got()
    {
        std::lock_guard<std::mutex> lock(mu_);
        return got_;
    }

    std::vector<uint8_t> ports()
    {
        std::lock_guard<std::mutex> lock(mu_);
        return ports_;
    }

    bool wait_for(size_t n, std::chrono::milliseconds timeout = 2000ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (got().size() >= n)
            {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return got().size() >= n;
    }

    std::atomic<bool>     blocked {false};
    std::atomic<uint64_t> attempts {0};
    bool                  reject_odd = false;
    int               open_result = 0;

private:
    std::string           tag_;
    call_log             *log_;
    std::mutex            mu_;
    std::vector<uint64_t> got_;
    std::vector<uint8_t>  ports_;
};

std::vector<uint64_t> iota_seq(uint64_t n)
{
    std::vector<uint64_t> v;
    for (uint64_t i = 0; i < n; i++)
    {
        v.push_back(i);
    }
    return v;
}

struct chain
{
    call_log    log;
    pipeline    p {"test"};
    fake_source *src = nullptr;
    fake_coder  *mid = nullptr;
    fake_sink   *snk = nullptr;

    explicit chain(uint64_t count)
    {
        auto s = std::make_unique<fake_source>("src", &log, count);
        auto c = std::make_unique<fake_coder>("mid", &log);
        auto k = std::make_unique<fake_sink>("snk", &log);
        src = s.get();
        mid = c.get();
        snk = k.get();
        EXPECT_EQ(0, p.add_source("src", std::move(s)));
        EXPECT_EQ(0, p.add_coder("mid", std::move(c)));
        EXPECT_EQ(0, p.add_sink("snk", std::move(k)));
        EXPECT_EQ(0, p.link_chain({"src", "mid", "snk"}));
    }
};

}  // namespace

TEST(PipelineTest, BuildRejectsBadNamesAndLinks)
{
    call_log log;
    pipeline p("build");
    EXPECT_EQ(0, p.add_source("src", std::make_unique<fake_source>("src", &log, 0)));
    EXPECT_EQ(-EEXIST, p.add_sink("src", std::make_unique<fake_sink>("dup", &log)));
    EXPECT_EQ(-EINVAL, p.add_sink("", std::make_unique<fake_sink>("empty", &log)));
    EXPECT_EQ(-ENODEV, p.add_coder("x", "no_such_component"));
    EXPECT_EQ(0, p.add_coder("a", std::make_unique<fake_coder>("a", &log)));
    EXPECT_EQ(0, p.add_coder("b", std::make_unique<fake_coder>("b", &log)));
    EXPECT_EQ(0, p.add_sink("snk", std::make_unique<fake_sink>("snk", &log)));

    EXPECT_EQ(-ENOENT, p.link("src", "nope"));
    EXPECT_EQ(-EINVAL, p.link("snk", "a"));   /* sink has no output */
    EXPECT_EQ(-EINVAL, p.link("a", "src"));   /* source has no input */
    EXPECT_EQ(0, p.link("src", "a"));
    EXPECT_EQ(-EEXIST, p.link("src", "a"));
    EXPECT_EQ(0, p.link("a", "b"));
    EXPECT_EQ(-ELOOP, p.link("b", "a"));
    EXPECT_EQ(-ELOOP, p.link("a", "a"));

    EXPECT_EQ((std::vector<std::string> {"src", "a", "b", "snk"}), p.node_names());
}

TEST(PipelineTest, ConfigureAndQueryByNodeName)
{
    chain c(0);
    EXPECT_EQ(0, c.p.configure("src", "label", "cam0"));
    std::string v;
    EXPECT_EQ(0, c.p.query("src", "label", &v));
    EXPECT_EQ("cam0", v);
    EXPECT_EQ(-ENOTSUP, c.p.configure("src", "nope", "1"));
    EXPECT_EQ(-ENOENT, c.p.configure("ghost", "label", "x"));
    EXPECT_EQ(-ENOENT, c.p.query("ghost", "label", &v));
    EXPECT_EQ(0, c.p.query("snk", "pipeline.rejects", &v));
    EXPECT_EQ("0", v);
    EXPECT_NE(nullptr, c.p.node("mid"));
    EXPECT_EQ(nullptr, c.p.node("ghost"));
}

TEST(PipelineTest, RunsChainInOrderAndOpensSinksFirst)
{
    chain c(100);
    ASSERT_EQ(0, c.p.start());
    EXPECT_EQ(pipeline_state_e::RUNNING, c.p.state());
    ASSERT_TRUE(c.snk->wait_for(100));
    EXPECT_EQ(iota_seq(100), c.snk->got());

    EXPECT_EQ(-EBUSY, c.p.add_sink("late", std::make_unique<fake_sink>("late", &c.log)));
    EXPECT_EQ(-EBUSY, c.p.link("src", "snk"));

    ASSERT_EQ(0, c.p.stop());
    EXPECT_EQ(pipeline_state_e::STOPPED, c.p.state());
    EXPECT_EQ((std::vector<std::string> {"open snk", "open mid", "open src", "close src",
                                         "close mid", "close snk"}),
              c.log.get());
}

TEST(PipelineTest, WakesOnNotifyWithoutPolling)
{
    chain c(0);
    ASSERT_EQ(0, c.p.start());
    c.src->add_more(5);
    ASSERT_TRUE(c.snk->wait_for(5, 200ms));
    EXPECT_EQ(iota_seq(5), c.snk->got());
}

TEST(PipelineTest, PauseHoldsDataAndStartResumes)
{
    chain c(0);
    ASSERT_EQ(0, c.p.pause());
    EXPECT_EQ(pipeline_state_e::PAUSED, c.p.state());
    c.src->add_more(10);
    std::this_thread::sleep_for(50ms);
    EXPECT_TRUE(c.snk->got().empty());

    ASSERT_EQ(0, c.p.start());
    ASSERT_TRUE(c.snk->wait_for(10));
    ASSERT_EQ(0, c.p.pause());
    EXPECT_EQ(0, c.p.pause());
    c.src->add_more(3);
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(10U, c.snk->got().size());
}

TEST(PipelineTest, BackpressureLosesNothing)
{
    chain c(50);
    c.snk->blocked = true;
    ASSERT_EQ(0, c.p.start());
    std::this_thread::sleep_for(30ms);
    EXPECT_TRUE(c.snk->got().empty());
    c.snk->blocked = false;
    ASSERT_TRUE(c.snk->wait_for(50));
    EXPECT_EQ(iota_seq(50), c.snk->got());
}

/* A source whose deadline has passed while its output link is held must not make the
 * pipeline thread spin: the held PDU is retried on a short timer instead. */
TEST(PipelineTest, HeldLinkWithPastDeadlineDoesNotSpin)
{
    chain c(5);
    c.src->deadline_ns = 1;
    c.snk->blocked = true;
    ASSERT_EQ(0, c.p.start());
    std::this_thread::sleep_for(20ms);
    const uint64_t before = c.snk->attempts.load();
    std::this_thread::sleep_for(100ms);
    const uint64_t retries = c.snk->attempts.load() - before;
    /* 200 us retry: about 500 in 100 ms. Waiting for the 50 ms idle cap gives about 2 (nodes
     * do not notify when input room frees); a spin makes far more. */
    EXPECT_GT(retries, 50U);
    EXPECT_LT(retries, 1000U);
    c.snk->blocked = false;
    ASSERT_TRUE(c.snk->wait_for(5));
}

/* A node whose deadline has passed (it fell due during a pass) and that does not notify must be
 * pulled again within about k_held_retry_ns, not after the 50 ms idle cap, and without spinning. */
TEST(PipelineTest, PastDeadlineOnPullableNodeIsNotMissed)
{
    chain c(1);
    c.src->ready = false;
    c.src->deadline_ns = 1;
    ASSERT_EQ(0, c.p.start());
    std::this_thread::sleep_for(20ms);
    const uint64_t before = c.src->output_calls.load();
    std::this_thread::sleep_for(100ms);
    const uint64_t polls = c.src->output_calls.load() - before;
    /* 200 us: about 500 in 100 ms. Missed deadlines give about 2; a spin far more. */
    EXPECT_GT(polls, 50U);
    EXPECT_LT(polls, 1000U);
    c.src->ready = true;
    ASSERT_TRUE(c.snk->wait_for(1));
}

TEST(PipelineTest, RejectedPdusAreCountedAndPipelineKeepsRunning)
{
    chain c(10);
    c.snk->reject_odd = true;
    ASSERT_EQ(0, c.p.start());
    ASSERT_TRUE(c.snk->wait_for(5));
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ((std::vector<uint64_t> {0, 2, 4, 6, 8}), c.snk->got());
    EXPECT_EQ(pipeline_state_e::RUNNING, c.p.state());
    std::string v;
    ASSERT_EQ(0, c.p.query("snk", "pipeline.rejects", &v));
    EXPECT_EQ("5", v);
}

TEST(PipelineTest, SourceDeviceLossKeepsRunning)
{
    chain c(3);
    c.src->set_idle_result(-ENODEV);
    ASSERT_EQ(0, c.p.start());
    ASSERT_TRUE(c.snk->wait_for(3));
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(pipeline_state_e::RUNNING, c.p.state());
}

TEST(PipelineTest, FatalErrorCallsHandlerOnceAndRestartWorks)
{
    chain c(10);
    c.mid->fail_at = 4;
    c.mid->fail_with = -EIO;

    std::mutex            mu;
    std::vector<std::string> calls;
    std::thread::id       handler_thread;
    c.p.set_error_handler([&](const std::string &node, int err) {
        std::lock_guard<std::mutex> lock(mu);
        calls.push_back(node + ":" + std::to_string(err));
        handler_thread = std::this_thread::get_id();
    });

    ASSERT_EQ(0, c.p.start());
    for (int i = 0; i < 200 && c.p.state() != pipeline_state_e::ERROR; i++)
    {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(pipeline_state_e::ERROR, c.p.state());
    std::this_thread::sleep_for(20ms);
    {
        std::lock_guard<std::mutex> lock(mu);
        EXPECT_EQ((std::vector<std::string> {"mid:" + std::to_string(-EIO)}), calls);
        EXPECT_NE(std::this_thread::get_id(), handler_thread);
    }
    /* PDUs still inside the coder when it fails never reach the sink. */
    const std::vector<uint64_t> before = c.snk->got();
    EXPECT_LE(before.size(), 4U);
    EXPECT_EQ(iota_seq(before.size()), before);

    std::string node;
    int         err = 0;
    c.p.last_error(&node, &err);
    EXPECT_EQ("mid", node);
    EXPECT_EQ(-EIO, err);

    EXPECT_EQ(-EIO, c.p.start());
    EXPECT_EQ(-EIO, c.p.pause());
    ASSERT_EQ(0, c.p.stop());
    c.p.last_error(&node, &err);
    EXPECT_EQ("mid", node);

    c.mid->fail_at = UINT64_MAX;
    ASSERT_EQ(0, c.p.start());
    c.p.last_error(&node, &err);
    EXPECT_EQ(0, err);
    /* The source resumes after the failed PDU (seq 4) and the rest flows. */
    bool saw_last = false;
    for (int i = 0; i < 2000 && !saw_last; i++)
    {
        const auto got = c.snk->got();
        saw_last = !got.empty() && got.back() == 9U;
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(saw_last);
    EXPECT_EQ(pipeline_state_e::RUNNING, c.p.state());
}

TEST(PipelineTest, ControlFromPipelineThreadIsRefused)
{
    chain c(2);
    c.mid->fail_at = 1;
    c.mid->fail_with = -EIO;
    std::atomic<int> stop_result {1};
    c.p.set_error_handler([&](const std::string &, int) { stop_result = c.p.stop(); });
    ASSERT_EQ(0, c.p.start());
    for (int i = 0; i < 200 && stop_result.load() == 1; i++)
    {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(-EDEADLK, stop_result.load());
    EXPECT_EQ(pipeline_state_e::ERROR, c.p.state());
}

TEST(PipelineTest, OpenFailureClosesOpenedNodesAndStaysStopped)
{
    call_log log;
    pipeline p("openfail");
    auto     bad = std::make_unique<fake_sink>("bad", &log);
    bad->open_result = -EACCES;
    /* Source-first order is src, mid, bad, snk, so opening goes snk, bad (fails). */
    ASSERT_EQ(0, p.add_source("src", std::make_unique<fake_source>("src", &log, 1)));
    ASSERT_EQ(0, p.add_coder("mid", std::make_unique<fake_coder>("mid", &log)));
    ASSERT_EQ(0, p.add_sink("bad", std::move(bad)));
    ASSERT_EQ(0, p.add_sink("snk", std::make_unique<fake_sink>("snk", &log)));
    ASSERT_EQ(0, p.link_chain({"src", "mid", "snk"}));
    ASSERT_EQ(0, p.link("mid", "bad"));

    EXPECT_EQ(-EACCES, p.start());
    EXPECT_EQ(pipeline_state_e::STOPPED, p.state());
    std::string node;
    int         err = 0;
    p.last_error(&node, &err);
    EXPECT_EQ("bad", node);
    EXPECT_EQ(-EACCES, err);
    EXPECT_EQ((std::vector<std::string> {"open snk", "open bad", "close snk"}), log.get());
    /* Graph is editable again. */
    EXPECT_EQ(0, p.add_sink("later", std::make_unique<fake_sink>("later", &log)));
}

TEST(PipelineTest, FanOutAndPortsReachEveryLink)
{
    call_log log;
    pipeline p("fan");
    auto     a = std::make_unique<fake_sink>("a", &log);
    auto     b = std::make_unique<fake_sink>("b", &log);
    fake_sink *pa = a.get();
    fake_sink *pb = b.get();
    ASSERT_EQ(0, p.add_source("src", std::make_unique<fake_source>("src", &log, 20)));
    ASSERT_EQ(0, p.add_sink("a", std::move(a)));
    ASSERT_EQ(0, p.add_sink("b", std::move(b)));
    ASSERT_EQ(0, p.link("src", "a"));
    ASSERT_EQ(0, p.link("src", 0, "b", 3));
    ASSERT_EQ(0, p.start());
    ASSERT_TRUE(pa->wait_for(20));
    ASSERT_TRUE(pb->wait_for(20));
    EXPECT_EQ(iota_seq(20), pa->got());
    EXPECT_EQ(iota_seq(20), pb->got());
    EXPECT_EQ(std::vector<uint8_t>(20, 0), pa->ports());
    EXPECT_EQ(std::vector<uint8_t>(20, 3), pb->ports());
}

TEST(PipelineQueueTest, DropsOldestDataButKeepsCaps)
{
    pipeline_queue q(3);
    vstreamer::video_raw_caps caps {};
    caps.width = 64;
    caps.height = 32;
    component_pdu cp = vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_RAW, caps, 0, 0);
    ASSERT_EQ(0, q.push(std::move(cp)));
    for (uint32_t i = 0; i < 5; i++)
    {
        ASSERT_EQ(0, q.push(vstreamer::test_pdu::make_stream_dgram(i, 8)));
    }
    EXPECT_EQ(3U, q.depth());
    EXPECT_EQ(6U, q.pushed());
    EXPECT_EQ(3U, q.drops());

    component_pdu out;
    ASSERT_EQ(0, q.pop(out));
    EXPECT_TRUE(vstreamer::is_caps(out.sdu_type));
    ASSERT_EQ(0, q.pop(out));
    EXPECT_EQ(3U, out.seq);
    ASSERT_EQ(0, q.pop(out));
    EXPECT_EQ(4U, out.seq);
    EXPECT_EQ(-EAGAIN, q.pop(out));

    ASSERT_EQ(0, q.push(vstreamer::test_pdu::make_stream_dgram(9, 8)));
    q.close();
    EXPECT_EQ(-ECANCELED, q.push(vstreamer::test_pdu::make_stream_dgram(10, 8)));
    ASSERT_EQ(0, q.pop(out));
    EXPECT_EQ(9U, out.seq);
    EXPECT_EQ(-ECANCELED, q.pop(out));
    q.reopen();
    EXPECT_EQ(-EAGAIN, q.pop(out));

    /* A restart drops what the previous producer session left behind. */
    ASSERT_EQ(0, q.push(vstreamer::test_pdu::make_stream_dgram(11, 8)));
    q.close();
    q.reopen();
    EXPECT_EQ(0U, q.depth());
    EXPECT_EQ(-EAGAIN, q.pop(out));

    EXPECT_EQ(-EINVAL, q.set_capacity(0));
    EXPECT_EQ(0, q.set_capacity(1));
    EXPECT_EQ(1U, q.capacity());
}

/* Restarting only the consumer pipeline: its nodes forgot the format and the producer will not
 * send caps again, so the queue replays the latest caps first. */
TEST(PipelineQueueTest, ConsumerRestartGetsCapsAgain)
{
    auto q = std::make_shared<pipeline_queue>(8);
    auto src = vstreamer::make_pipeline_queue_source(q);
    vstreamer::video_raw_caps caps {};
    caps.width = 64;
    caps.height = 32;
    ASSERT_EQ(0, q->push(vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_RAW, caps, 0, 0)));
    ASSERT_EQ(0, q->push(vstreamer::test_pdu::make_stream_dgram(1, 8)));

    ASSERT_EQ(0, src->open());
    component_pdu out;
    ASSERT_EQ(0, src->output(out));
    EXPECT_TRUE(vstreamer::is_caps(out.sdu_type));
    ASSERT_EQ(0, src->output(out));
    EXPECT_EQ(1U, out.seq);
    src->close();

    ASSERT_EQ(0, q->push(vstreamer::test_pdu::make_stream_dgram(2, 8)));
    ASSERT_EQ(0, src->open());
    ASSERT_EQ(0, src->output(out));
    EXPECT_TRUE(vstreamer::is_caps(out.sdu_type));
    ASSERT_EQ(0, src->output(out));
    EXPECT_EQ(2U, out.seq);

    /* Caps already next: not doubled. */
    src->close();
    ASSERT_EQ(0, src->open());
    ASSERT_EQ(0, src->output(out));
    EXPECT_TRUE(vstreamer::is_caps(out.sdu_type));
    EXPECT_EQ(-EAGAIN, src->output(out));
    src->close();
    ASSERT_EQ(0, src->open());
    ASSERT_EQ(0, src->output(out));
    EXPECT_TRUE(vstreamer::is_caps(out.sdu_type));
    EXPECT_EQ(-EAGAIN, src->output(out));

    /* A new producer session forgets the old caps. */
    q->reopen();
    src->close();
    ASSERT_EQ(0, src->open());
    EXPECT_EQ(-EAGAIN, src->output(out));
}

TEST(PipelineQueueTest, TwoPipelinesThroughQueue)
{
    call_log log;
    auto     q = std::make_shared<pipeline_queue>(256);

    pipeline prod("prod");
    pipeline cons("cons");
    auto     src = std::make_unique<fake_source>("src", &log, 0);
    auto     snk = std::make_unique<fake_sink>("snk", &log);
    fake_source *psrc = src.get();
    fake_sink   *psnk = snk.get();
    ASSERT_EQ(0, prod.add_source("src", std::move(src)));
    ASSERT_EQ(0, prod.add_queue_sink("to_cons", q));
    ASSERT_EQ(0, prod.link("src", "to_cons"));
    ASSERT_EQ(0, cons.add_queue_source("from_prod", q));
    ASSERT_EQ(0, cons.add_sink("snk", std::move(snk)));
    ASSERT_EQ(0, cons.link("from_prod", "snk"));

    ASSERT_EQ(0, cons.start());
    ASSERT_EQ(0, prod.start());
    psrc->add_more(200);
    ASSERT_TRUE(psnk->wait_for(200));
    EXPECT_EQ(iota_seq(200), psnk->got());

    std::string v;
    ASSERT_EQ(0, cons.query("from_prod", "pushed", &v));
    EXPECT_EQ("200", v);
    ASSERT_EQ(0, prod.configure("to_cons", "capacity", "8"));
    ASSERT_EQ(0, cons.query("from_prod", "capacity", &v));
    EXPECT_EQ("8", v);
    EXPECT_EQ(-EINVAL, prod.configure("to_cons", "capacity", "0"));

    /* Stopping the producer is not an error for the consumer. */
    ASSERT_EQ(0, prod.stop());
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(pipeline_state_e::RUNNING, cons.state());
}

#if defined(ENABLE_SDL_SINK)
/* RX layout: the display gets its own pipeline so the vsync wait inside sdl_sink::input() does
 * not stall the nodes before it. */
TEST(PipelineQueueTest, SdlSinkInDisplayPipeline)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    call_log log;
    auto     q = std::make_shared<pipeline_queue>(1);

    pipeline rx("rx");
    pipeline display("display");
    auto     src = std::make_unique<fake_source>("src", &log, 0);
    ASSERT_EQ(0, rx.add_source("src", std::move(src)));
    ASSERT_EQ(0, rx.add_queue_sink("to_display", q));
    ASSERT_EQ(0, rx.link("src", "to_display"));
    ASSERT_EQ(0, display.add_queue_source("from_rx", q));
    ASSERT_EQ(0, display.add_sink("display", "sdl_sink"));
    ASSERT_EQ(0, display.configure("display", "video_driver", "dummy"));
    ASSERT_EQ(0, display.link("from_rx", "display"));

    ASSERT_EQ(0, display.start());
    ASSERT_EQ(0, rx.start());

    /* Frames pushed straight into the queue, as a decoder output would arrive. */
    vstreamer::video_raw_caps caps {};
    caps.width = 64;
    caps.height = 32;
    caps.hor_stride = 64;
    caps.ver_stride = 32;
    ASSERT_EQ(0, q->push(vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_RAW, caps, 0, 0)));
    for (int i = 0; i < 5; i++)
    {
        ASSERT_EQ(0, q->push(vstreamer::test_pdu::make_nv12(64, 32, 1000U + static_cast<uint64_t>(i))));
        std::this_thread::sleep_for(5ms);
    }

    std::string stats;
    bool        presented = false;
    for (int i = 0; i < 500 && !presented; i++)
    {
        ASSERT_EQ(0, display.query("display", "stats", &stats));
        presented = stats.rfind("frames=0", 0) != 0;
        std::this_thread::sleep_for(2ms);
    }
    EXPECT_TRUE(presented) << stats;
    EXPECT_EQ(pipeline_state_e::RUNNING, display.state());
    ASSERT_EQ(0, display.query("display", "pipeline.rejects", &stats));
    EXPECT_EQ("0", stats);
    ASSERT_EQ(0, rx.stop());
    ASSERT_EQ(0, display.stop());
}
#endif

#if defined(ENABLE_NOISE_SOURCE)
TEST(PipelineTest, NoiseSourceByFactoryName)
{
    call_log log;
    pipeline p("noise");
    auto     snk = std::make_unique<fake_sink>("snk", &log);
    fake_sink *psnk = snk.get();
    ASSERT_EQ(0, p.add_source("noise", "noise_source"));
    ASSERT_EQ(0, p.add_sink("snk", std::move(snk)));
    ASSERT_EQ(0, p.link("noise", "snk"));
    ASSERT_EQ(0, p.configure("noise", "size", "64x32"));
    ASSERT_EQ(0, p.configure("noise", "fps", "100"));
    ASSERT_EQ(0, p.start());
    /* caps PDU + frames, paced by next_deadline_ns() */
    EXPECT_TRUE(psnk->wait_for(5));
    std::string v;
    EXPECT_EQ(0, p.query("noise", "size", &v));
    EXPECT_EQ("64x32", v);
}
#endif
