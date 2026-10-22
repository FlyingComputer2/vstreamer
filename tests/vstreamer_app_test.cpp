#include "apps/vstreamer/mplane.hpp"
#include "apps/vstreamer/pipeline_set.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "components/components_config.hpp"
#include "core/component_coder.hpp"
#include "core/component_sink.hpp"
#include "core/component_source.hpp"
#include "test_pdu_helpers.hpp"

namespace
{

using namespace std::chrono_literals;
using vstreamer::component_pdu;
using vstreamer::pipeline_state_e;
using vstreamer::app::pipeline_set;

/* Shared by the fakes of one test: open order and a per-type count of created nodes. */
struct registry
{
    std::mutex               mu;
    std::vector<std::string> opens;

    void opened(const std::string &tag)
    {
        std::lock_guard<std::mutex> lock(mu);
        opens.push_back(tag);
    }

    std::vector<std::string> get_opens()
    {
        std::lock_guard<std::mutex> lock(mu);
        return opens;
    }
};

/* A "label" key every fake accepts, so files can configure them. */
class labelled
{
protected:
    int set_label(std::string_view key, std::string_view value)
    {
        if ("label" != key)
        {
            return -ENOTSUP;
        }
        std::lock_guard<std::mutex> lock(label_mu_);
        label_ = std::string(value);
        return 0;
    }

    int get_label(std::string_view key, std::string *value) const
    {
        if ("label" != key)
        {
            return -ENOTSUP;
        }
        std::lock_guard<std::mutex> lock(label_mu_);
        *value = label_;
        return 0;
    }

private:
    mutable std::mutex label_mu_;
    std::string        label_;
};

/* Emits one PDU per ms with increasing seq while open. */
class fake_source : public vstreamer::component_source, labelled
{
public:
    explicit fake_source(registry *r) : r_(r) {}
    [[nodiscard]] std::string name() const override { return "fake_source"; }
    int open() override
    {
        std::string label;
        (void)get_label("label", &label);
        r_->opened(label);
        return 0;
    }
    void close() override {}
    int  output(component_pdu &out) override
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_)
        {
            return -EAGAIN;
        }
        next_ = now + 1ms;
        out = vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(seq_), 16);
        out.seq = seq_++;
        return 0;
    }
    [[nodiscard]] int64_t next_deadline_ns() const override
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(next_.time_since_epoch())
            .count();
    }
    int configure(std::string_view k, std::string_view v) override { return set_label(k, v); }
    int query(std::string_view k, std::string *v) const override { return get_label(k, v); }

private:
    registry                             *r_;
    uint64_t                              seq_ = 0;
    std::chrono::steady_clock::time_point next_ {};
};

/* Passes PDUs through; fails with -EIO on the PDU with seq == fail_at (set via label "fail"). */
class fake_coder : public vstreamer::component_coder, labelled
{
public:
    explicit fake_coder(registry *r, std::atomic<int> *fails_left) : r_(r), fails_left_(fails_left) {}
    [[nodiscard]] std::string name() const override { return "fake_coder"; }
    int open() override
    {
        std::string label;
        (void)get_label("label", &label);
        r_->opened(label);
        q_.clear();
        return 0;
    }
    void close() override {}
    int  input(component_pdu &&in) override
    {
        if (in.seq >= 5 && nullptr != fails_left_ && fails_left_->load() > 0)
        {
            fails_left_->fetch_sub(1);
            return -EIO;
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
    int configure(std::string_view k, std::string_view v) override { return set_label(k, v); }
    int query(std::string_view k, std::string *v) const override { return get_label(k, v); }

private:
    registry                 *r_;
    std::atomic<int>         *fails_left_;
    std::deque<component_pdu> q_;
};

class fake_sink : public vstreamer::component_sink, labelled
{
public:
    explicit fake_sink(registry *r, std::atomic<uint64_t> *got) : r_(r), got_(got) {}
    [[nodiscard]] std::string name() const override { return "fake_sink"; }
    int open() override
    {
        std::string label;
        (void)get_label("label", &label);
        r_->opened(label);
        return 0;
    }
    void close() override {}
    int  input(component_pdu &&) override
    {
        got_->fetch_add(1);
        return 0;
    }
    int configure(std::string_view k, std::string_view v) override { return set_label(k, v); }
    int query(std::string_view k, std::string *v) const override { return get_label(k, v); }

private:
    registry              *r_;
    std::atomic<uint64_t> *got_;
};

struct fixture
{
    registry              reg;
    std::atomic<uint64_t> sink_got {0};
    std::atomic<int>      coder_fails {0};
    pipeline_set          set;

    fixture()
    {
        set.set_node_factory([this](std::string_view type) {
            pipeline_set::created_node c;
            if ("src" == type)
            {
                c.source = std::make_unique<fake_source>(&reg);
            }
            else if ("pass" == type)
            {
                c.coder = std::make_unique<fake_coder>(&reg, &coder_fails);
            }
            else if ("snk" == type)
            {
                c.sink = std::make_unique<fake_sink>(&reg, &sink_got);
            }
            return c;
        });
    }

    std::string load(const char *text)
    {
        std::string err;
        (void)set.load_text(text, "t.vsp", &err);
        return err;
    }

    std::string mplane(const char *line)
    {
        std::string reply;
        EXPECT_TRUE(vstreamer::app::handle_mplane_line(set, line, reply)) << line;
        return reply;
    }

    bool wait_got(uint64_t n)
    {
        for (int i = 0; i < 2000 && sink_got.load() < n; i++)
        {
            std::this_thread::sleep_for(1ms);
        }
        return sink_got.load() >= n;
    }
};

constexpr const char k_two_pipes[] =
    "# two pipelines through a queue\n"
    "queue q 4\n"
    "\n"
    "pipeline tx\n"
    "node a src label=tx-src\n"
    "node b pass label=tx-pass   # inline comment\n"
    "link a -> b -> @q\n"
    "\n"
    "pipeline rx\n"
    "node s snk label=rx-snk\n"
    "link @q -> s\n";

std::string load_error(const char *text)
{
    fixture f;
    return f.load(text);
}

}  // namespace

TEST(VstreamerAppTest, FileErrorsNameTheLine)
{
    EXPECT_EQ("t.vsp:1: unknown command 'nodes'", load_error("nodes a src\n"));
    EXPECT_EQ("t.vsp:1: node outside a pipeline", load_error("node a src\n"));
    EXPECT_EQ("t.vsp:2: unknown or not built component type 'nope'",
              load_error("pipeline p\nnode a nope\n"));
    EXPECT_EQ("t.vsp:3: node 'a' already declared",
              load_error("pipeline p\nnode a src\nnode a snk\n"));
    EXPECT_EQ("t.vsp:2: expected key=value, got 'label'",
              load_error("pipeline p\nnode a src label\n"));
    EXPECT_EQ("t.vsp:2: set a color 'red' failed (ENOTSUP)",
              load_error("pipeline p\nnode a src color=red\n"));
    EXPECT_EQ("t.vsp:1: unknown node 'ghost'", load_error("set ghost k v\n"));
    EXPECT_EQ("t.vsp:3: unknown node 'c'", load_error("pipeline p\nnode a src\nlink a -> c\n"));
    EXPECT_EQ("t.vsp:3: expected '->', got '=>'",
              load_error("pipeline p\nnode a src\nlink a => b\n"));
    EXPECT_EQ("t.vsp:5: node 'a' is in pipeline 'p'; use a queue to cross pipelines",
              load_error("pipeline p\nnode a src\npipeline r\nnode s snk\nlink a -> s\n"));
    EXPECT_EQ("t.vsp:4: cannot link s -> a: 's' has no output or 'a' has no input",
              load_error("pipeline p\nnode a src\nnode s snk\nlink s -> a\n"));
    EXPECT_EQ("t.vsp:2: queue capacity must be 1..1024", load_error("pipeline p\nqueue q 0\n"));
    EXPECT_EQ("t.vsp:4: unknown queue 'nq'",
              load_error("queue q\npipeline p\nnode a src\nlink a -> @nq\n"));
    EXPECT_EQ("t.vsp:5: queue 'q' must start or end a link",
              load_error("queue q\npipeline p\nnode a src\nnode s snk\nlink a -> @q -> s\n"));
    EXPECT_EQ("t.vsp:6: queue 'q' would read and write in the same pipeline",
              load_error("queue q\npipeline p\nnode a src\nnode s snk\nlink a -> @q\n"
                         "link @q -> s\n"));
    EXPECT_EQ("t.vsp:10: queue 'q' already has a reader in pipeline 'r'",
              load_error("queue q\npipeline p\nnode a src\nlink a -> @q\npipeline r\n"
                         "node s snk\nlink @q -> s\npipeline x\nnode t snk\nlink @q -> t\n"));
    /* Checked after the last line, reported at the queue's declaration. */
    EXPECT_EQ("t.vsp:1: queue 'q' needs a writer and a reader",
              load_error("queue q\npipeline p\nnode a src\nnode s snk\nlink a -> @q\n"));
    EXPECT_EQ("t.vsp:1: no pipeline declared", load_error("# empty\n"));
    EXPECT_EQ("t.vsp:3: unknown command 'enable'", load_error("pipeline p\nnode a src\nenable a\n"));
}

TEST(VstreamerAppTest, SetTakesRestOfLineAndCommentsAreIgnored)
{
    fixture f;
    ASSERT_EQ("", f.load("pipeline p\n"
                         "node a src\n"
                         "set a label two words # not part of it\n"
                         "node b snk label=x#y\n"
                         "link a -> b\n"));
    std::string v;
    ASSERT_EQ(0, f.set.query("a", "label", &v));
    EXPECT_EQ("two words", v);
    ASSERT_EQ(0, f.set.query("b", "label", &v));
    EXPECT_EQ("x#y", v);
}

TEST(VstreamerAppTest, StartsReadersFirstAndMovesData)
{
    fixture f;
    ASSERT_EQ("", f.load(k_two_pipes));
    ASSERT_EQ(0, f.set.start("all"));
    /* rx reads the queue tx writes, so it opens first. */
    const auto opens = f.reg.get_opens();
    ASSERT_EQ(3U, opens.size());
    EXPECT_EQ("rx-snk", opens[0]);
    EXPECT_TRUE(f.wait_got(20));
    ASSERT_EQ(0, f.set.stop("all"));
    for (const auto &[name, state] : f.set.states())
    {
        EXPECT_EQ(pipeline_state_e::STOPPED, state) << name;
    }
}

TEST(VstreamerAppTest, MplaneVerbs)
{
    fixture f;
    ASSERT_EQ("", f.load(k_two_pipes));

    EXPECT_EQ("ok tx-pass\n", f.mplane("get b label"));
    EXPECT_EQ("ok\n", f.mplane("set b label new value"));
    EXPECT_EQ("ok new value\n", f.mplane("get b label"));
    EXPECT_EQ("err -95 ENOTSUP\n", f.mplane("get b nope"));
    EXPECT_EQ("err -2 ENOENT\n", f.mplane("get ghost label"));
    EXPECT_EQ("err -2 ENOENT\n", f.mplane("set ghost label x"));
    EXPECT_EQ("err usage: get <node> <key>\n", f.mplane("get b"));
    EXPECT_EQ("err usage: set <node> <key> <value>\n", f.mplane("set b label"));
    EXPECT_EQ("ok 4\n", f.mplane("get @q capacity"));

    EXPECT_EQ("ok tx a src\nok tx b pass\nok tx @q queue_sink\nok rx s snk\nok rx @q queue_source\n",
              f.mplane("nodes"));
    EXPECT_EQ("ok tx stopped\nok rx stopped\n", f.mplane("state"));
    EXPECT_EQ("ok\n", f.mplane("start"));
    EXPECT_EQ("ok tx running\nok rx running\n", f.mplane("state"));
    EXPECT_EQ("ok\n", f.mplane("pause tx"));
    EXPECT_EQ("ok tx paused\nok rx running\n", f.mplane("state"));
    EXPECT_EQ("err -2 ENOENT\n", f.mplane("stop nope"));
    EXPECT_EQ("err usage: stop [<pipeline>|all]\n", f.mplane("stop tx rx"));
    EXPECT_EQ("ok\n", f.mplane("stop all"));
    EXPECT_EQ("ok tx stopped\nok rx stopped\n", f.mplane("state"));

    std::string reply;
    EXPECT_FALSE(vstreamer::app::handle_mplane_line(f.set, "get", reply));
    EXPECT_FALSE(vstreamer::app::handle_mplane_line(f.set, "ping", reply));
}

TEST(VstreamerAppTest, SupervisorRestartsToWantedState)
{
    fixture f;
    ASSERT_EQ("", f.load("queue q 4\n"
                         "pipeline tx\nnode a src\nnode b pass\nlink a -> b -> @q\n"
                         "pipeline rx\nnode s snk\nlink @q -> s\n"
                         "pipeline idle\nnode i src\nnode j snk\nlink i -> j\n"));
    std::atomic<bool> run {true};
    std::thread       sup([&] { f.set.supervise(run, 50ms); });

    ASSERT_EQ(0, f.set.start("all"));
    ASSERT_EQ(0, f.set.stop("idle"));
    f.coder_fails = 1; /* b fails once on seq 5 */

    bool restarted = false;
    for (int i = 0; i < 3000 && !restarted; i++)
    {
        restarted = f.set.restarts() >= 1 && f.coder_fails.load() == 0;
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(restarted);
    for (int i = 0; i < 1000; i++)
    {
        bool ok = true;
        for (const auto &[name, state] : f.set.states())
        {
            ok = ok && state == ("idle" == name ? pipeline_state_e::STOPPED : pipeline_state_e::RUNNING);
        }
        if (ok)
        {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    for (const auto &[name, state] : f.set.states())
    {
        EXPECT_EQ("idle" == name ? pipeline_state_e::STOPPED : pipeline_state_e::RUNNING, state)
            << name;
    }
    const uint64_t before = f.sink_got.load();
    EXPECT_TRUE(f.wait_got(before + 10));

    run = false;
    f.set.wake_supervisor();
    sup.join();
}

#if defined(ENABLE_V4L2_SOURCE) && defined(ENABLE_JPEG_DECODER_MULTICORE) &&                   \
    (defined(ENABLE_H264_ENCODER_MPP) || defined(ENABLE_H264_ENCODER_CEDAR) ||                  \
     defined(ENABLE_H264_ENCODER_INTEL)) &&                                                     \
    defined(ENABLE_RTP_H264_PAY) && defined(ENABLE_STREAM_SENDER)
TEST(VstreamerAppTest, RoverExampleLoads)
{
    pipeline_set set;
    std::string  err;
    ASSERT_EQ(0, set.load_file(VSTREAMER_APP_EXAMPLES_DIR "/rover_tx.vsp", &err)) << err;
    EXPECT_EQ(7U, set.nodes().size());
}
#endif

#if defined(ENABLE_STREAM_RECEIVER) && defined(ENABLE_RTP_H264_DEPAY) &&                        \
    defined(ENABLE_H264_DECODER_MPP) && defined(ENABLE_SDL_SINK)
TEST(VstreamerAppTest, GroundStationExampleLoads)
{
    pipeline_set set;
    std::string  err;
    ASSERT_EQ(0, set.load_file(VSTREAMER_APP_EXAMPLES_DIR "/gs_rx.vsp", &err)) << err;
    EXPECT_EQ(6U, set.nodes().size());
}
#endif

#if defined(ENABLE_NOISE_SOURCE) && defined(ENABLE_STREAM_RECEIVER) &&                         \
    defined(ENABLE_STREAM_SENDER) && defined(ENABLE_RTP_H264_PAY) &&                            \
    defined(ENABLE_RTP_H264_DEPAY) && defined(ENABLE_H264_DECODER_MPP) &&                       \
    defined(ENABLE_SDL_SINK) &&                                                                 \
    (defined(ENABLE_H264_ENCODER_MPP) || defined(ENABLE_H264_ENCODER_CEDAR) ||                  \
     defined(ENABLE_H264_ENCODER_INTEL))
TEST(VstreamerAppTest, NoiseLoopbackExampleLoads)
{
    pipeline_set set;
    std::string  err;
    ASSERT_EQ(0, set.load_file(VSTREAMER_APP_EXAMPLES_DIR "/noise_loopback.vsp", &err)) << err;
    EXPECT_EQ(10U, set.nodes().size());
}
#endif

TEST(VstreamerAppTest, MissingFileIsAnError)
{
    pipeline_set set;
    std::string  err;
    EXPECT_EQ(-ENOENT, set.load_file("/nonexistent/x.vsp", &err));
    EXPECT_EQ(0U, err.rfind("/nonexistent/x.vsp: cannot open", 0)) << err;
}
