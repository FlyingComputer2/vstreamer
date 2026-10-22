#include "core/default_reactor_host.hpp"
#include "apps/vstreamer/pipeline_set.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

#include "core/component_coder.hpp"
#include "core/component_factory.hpp"
#include "core/component_sink.hpp"
#include "core/component_source.hpp"
#include "core/key_util.hpp"

namespace vstreamer::app
{
namespace
{

constexpr size_t k_default_queue_capacity = 8;

enum class want_e : uint8_t
{
    STOPPED,
    PAUSED,
    RUNNING,
};

struct token
{
    std::string_view text;
    size_t           offset = 0;
};

/* Drops a comment: '#' at the start of the line or after whitespace. */
std::string_view strip_comment(std::string_view line)
{
    for (size_t i = 0; i < line.size(); i++)
    {
        if ('#' == line[i] && (0 == i || ' ' == line[i - 1] || '\t' == line[i - 1]))
        {
            return line.substr(0, i);
        }
    }
    return line;
}

std::vector<token> tokenize(std::string_view line)
{
    std::vector<token> out;
    size_t             i = 0;
    while (i < line.size())
    {
        while (i < line.size() && (' ' == line[i] || '\t' == line[i] || '\r' == line[i]))
        {
            i++;
        }
        const size_t start = i;
        while (i < line.size() && ' ' != line[i] && '\t' != line[i] && '\r' != line[i])
        {
            i++;
        }
        if (i > start)
        {
            out.push_back({line.substr(start, i - start), start});
        }
    }
    return out;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (' ' == s.front() || '\t' == s.front()))
    {
        s.remove_prefix(1);
    }
    while (!s.empty() && (' ' == s.back() || '\t' == s.back() || '\r' == s.back()))
    {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] bool valid_name(std::string_view s)
{
    if (s.empty() || '@' == s.front())
    {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               '_' == c || '-' == c || '.' == c;
    });
}

std::string err_text(int err)
{
    return std::string(errno_name(err));
}

}  // namespace

const char *errno_name(int err)
{
    const int e = err < 0 ? -err : err;
    /* Same value as EOPNOTSUPP on Linux; components document it as -ENOTSUP. */
    if (ENOTSUP == e)
    {
        return "ENOTSUP";
    }
    const char *n = strerrorname_np(e);
    return (nullptr != n) ? n : "E?";
}

struct pipeline_set::impl
{
    struct pipe_entry
    {
        std::string               name;
        std::unique_ptr<pipeline> p;
        want_e                    want = want_e::STOPPED;
    };

    struct queue_entry
    {
        std::string                     name;
        std::shared_ptr<pipeline_queue> q;
        pipe_entry                     *writer = nullptr;
        pipe_entry                     *reader = nullptr;
        int                             line = 0;
    };

    struct node_rec
    {
        pipe_entry *pipe = nullptr;
        std::string name;
        std::string type;
    };

    std::unique_ptr<default_reactor_host> sender_reactor;
    node_factory_fn                          factory;
    std::vector<std::unique_ptr<pipe_entry>> pipes;
    std::vector<queue_entry>                 queues;
    std::vector<node_rec>                    nodes;
    /* Readers first. */
    std::vector<pipe_entry *> start_order;
    bool                      loaded = false;

    /* Serializes control calls from the mplane and the supervisor. */
    std::mutex ctl_mu;

    std::mutex              fail_mu;
    std::condition_variable fail_cv;
    bool                    failure_pending = false;
    std::string             fail_pipe;
    std::string             fail_node;
    int                     fail_err = 0;
    std::atomic<uint64_t>   restart_count {0};

    [[nodiscard]] pipe_entry *find_pipe(std::string_view name) const
    {
        for (const auto &p : pipes)
        {
            if (p->name == name)
            {
                return p.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] queue_entry *find_queue(std::string_view name)
    {
        for (auto &q : queues)
        {
            if (q.name == name)
            {
                return &q;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const node_rec *find_node(std::string_view name) const
    {
        for (const auto &n : nodes)
        {
            if (n.name == name)
            {
                return &n;
            }
        }
        return nullptr;
    }

    void on_pipeline_error(const std::string &pipe, const std::string &node, int err)
    {
        {
            std::lock_guard<std::mutex> lock(fail_mu);
            if (!failure_pending)
            {
                fail_pipe = pipe;
                fail_node = node;
                fail_err = err;
            }
            failure_pending = true;
        }
        fail_cv.notify_all();
    }

    void compute_start_order();
    int  apply(pipe_entry &p, want_e w);
    int  control(std::string_view target, want_e w);
    void clear();
};

/* Readers first: a pipeline that reads a queue starts before the one writing it. Pipelines in a
 * queue cycle keep file order. */
void pipeline_set::impl::compute_start_order()
{
    start_order.clear();
    std::vector<pipe_entry *> left;
    for (const auto &p : pipes)
    {
        left.push_back(p.get());
    }
    while (!left.empty())
    {
        auto ready = std::find_if(left.begin(), left.end(), [this, &left](pipe_entry *p) {
            /* Ready when every pipeline it writes to is already placed. */
            return std::none_of(queues.begin(), queues.end(), [&](const queue_entry &q) {
                return q.writer == p && q.reader != p &&
                       std::find(left.begin(), left.end(), q.reader) != left.end();
            });
        });
        if (ready == left.end())
        {
            ready = left.begin();
        }
        start_order.push_back(*ready);
        left.erase(ready);
    }
}

/* A start or pause that fails (a node did not open) is reported like a runtime error, so the
 * supervisor keeps retrying towards the wanted state (e.g. camera not plugged in yet). */
int pipeline_set::impl::apply(pipe_entry &p, want_e w)
{
    p.want = w;
    if (want_e::STOPPED == w)
    {
        return p.p->stop();
    }
    const int r = (want_e::RUNNING == w) ? p.p->start() : p.p->pause();
    if (r < 0 && -EIO != r)
    {
        std::string node;
        int         err = 0;
        p.p->last_error(&node, &err);
        on_pipeline_error(p.name, node, r);
    }
    return r;
}

int pipeline_set::impl::control(std::string_view target, want_e w)
{
    std::lock_guard<std::mutex> lock(ctl_mu);
    if (!target.empty() && "all" != target)
    {
        pipe_entry *p = find_pipe(target);
        return (nullptr != p) ? apply(*p, w) : -ENOENT;
    }
    int first_err = 0;
    auto run_one = [&](pipe_entry *p) {
        const int r = apply(*p, w);
        if (r < 0 && 0 == first_err)
        {
            first_err = r;
        }
    };
    if (want_e::STOPPED == w)
    {
        std::for_each(start_order.rbegin(), start_order.rend(), run_one);
    }
    else
    {
        std::for_each(start_order.begin(), start_order.end(), run_one);
    }
    return first_err;
}

void pipeline_set::impl::clear()
{
    for (auto it = start_order.rbegin(); it != start_order.rend(); ++it)
    {
        (*it)->p->stop();
    }
    start_order.clear();
    nodes.clear();
    queues.clear();
    pipes.clear();
}

namespace
{

/* Parses and builds one file into an impl. */
class loader
{
public:
    using impl = pipeline_set::impl;

    loader(impl &s, std::string_view source) : s_(s), source_(source) {}

    int run(std::string_view text, std::string *err)
    {
        std::istringstream in {std::string(text)};
        std::string        raw;
        while (std::getline(in, raw))
        {
            line_no_++;
            const std::string_view line = strip_comment(raw);
            const auto             toks = tokenize(line);
            if (toks.empty())
            {
                continue;
            }
            if (line_cmd(line, toks) < 0)
            {
                *err = message_;
                return -EINVAL;
            }
        }
        if (finish() < 0)
        {
            *err = message_;
            return -EINVAL;
        }
        return 0;
    }

private:
    int fail(int line, const std::string &msg)
    {
        message_ = std::string(source_) + ":" + std::to_string(line) + ": " + msg;
        return -EINVAL;
    }

    int fail(const std::string &msg) { return fail(line_no_, msg); }

    int line_cmd(std::string_view line, const std::vector<token> &t)
    {
        const std::string_view cmd = t[0].text;
        if ("queue" == cmd)
        {
            return cmd_queue(t);
        }
        if ("pipeline" == cmd)
        {
            return cmd_pipeline(t);
        }
        if ("node" == cmd)
        {
            return cmd_node(t);
        }
        if ("link" == cmd)
        {
            return cmd_link(t);
        }
        if ("set" == cmd)
        {
            return cmd_set(line, t);
        }
        return fail("unknown command '" + std::string(cmd) + "'");
    }

    int cmd_queue(const std::vector<token> &t)
    {
        if (t.size() < 2 || t.size() > 3)
        {
            return fail("usage: queue <name> [capacity]");
        }
        const std::string name(t[1].text);
        if (!valid_name(name))
        {
            return fail("bad queue name '" + name + "'");
        }
        if (nullptr != s_.find_queue(name))
        {
            return fail("queue '" + name + "' already declared");
        }
        size_t capacity = k_default_queue_capacity;
        if (3 == t.size())
        {
            int64_t v = 0;
            if (key_parse_i64(t[2].text, &v) != 0 || v < 1 || v > 1024)
            {
                return fail("queue capacity must be 1..1024");
            }
            capacity = static_cast<size_t>(v);
        }
        impl::queue_entry q;
        q.name = name;
        q.q = std::make_shared<pipeline_queue>(capacity);
        q.line = line_no_;
        s_.queues.push_back(std::move(q));
        return 0;
    }

    int cmd_pipeline(const std::vector<token> &t)
    {
        if (t.size() < 2 || t.size() > 3)
        {
            return fail("usage: pipeline <name> [cpu=<n>]");
        }
        const std::string name(t[1].text);
        if (!valid_name(name))
        {
            return fail("bad pipeline name '" + name + "'");
        }
        if (nullptr != s_.find_pipe(name))
        {
            return fail("pipeline '" + name + "' already declared");
        }
        auto p = std::make_unique<impl::pipe_entry>();
        p->name = name;
        p->p = std::make_unique<pipeline>(name);
        if (3 == t.size())
        {
            const std::string_view opt = t[2].text;
            int64_t                cpu = -1;
            if (opt.substr(0, 4) != "cpu=" || key_parse_i64(opt.substr(4), &cpu) != 0 ||
                cpu < -1 || cpu > 1023)
            {
                return fail("expected cpu=<n>, got '" + std::string(opt) + "'");
            }
            (void)p->p->set_cpu(static_cast<int>(cpu));
        }
        impl *s = &s_;
        p->p->set_error_handler([s, name](const std::string &node, int err) {
            s->on_pipeline_error(name, node, err);
        });
        cur_ = p.get();
        s_.pipes.push_back(std::move(p));
        return 0;
    }

    int cmd_node(const std::vector<token> &t)
    {
        if (nullptr == cur_)
        {
            return fail("node outside a pipeline");
        }
        if (t.size() < 3)
        {
            return fail("usage: node <name> <factory_type> [key=value ...]");
        }
        const std::string name(t[1].text);
        const std::string type(t[2].text);
        if (!valid_name(name))
        {
            return fail("bad node name '" + name + "'");
        }
        if (nullptr != s_.find_node(name))
        {
            return fail("node '" + name + "' already declared");
        }
        pipeline_set::created_node c = s_.factory(type);
        int                        r = -ENODEV;
        if (c.source)
        {
            r = cur_->p->add_source(name, std::move(c.source));
        }
        else if (c.coder)
        {
            r = cur_->p->add_coder(name, std::move(c.coder));
        }
        else if (c.sink)
        {
            r = cur_->p->add_sink(name, std::move(c.sink));
        }
        if (-ENODEV == r)
        {
            return fail("unknown or not built component type '" + type + "'");
        }
        if (r < 0)
        {
            return fail("cannot add node '" + name + "' (" + err_text(r) + ")");
        }
        s_.nodes.push_back({cur_, name, type});
        for (size_t i = 3; i < t.size(); i++)
        {
            const std::string_view kv = t[i].text;
            const size_t           eq = kv.find('=');
            if (std::string_view::npos == eq || 0 == eq)
            {
                return fail("expected key=value, got '" + std::string(kv) + "'");
            }
            if (configure(name, kv.substr(0, eq), kv.substr(eq + 1)) < 0)
            {
                return -EINVAL;
            }
        }
        return 0;
    }

    int configure(std::string_view node, std::string_view key, std::string_view value)
    {
        const impl::node_rec *n = s_.find_node(node);
        if (nullptr == n)
        {
            return fail("unknown node '" + std::string(node) + "'");
        }
        const int r = n->pipe->p->configure(node, key, value);
        if (r < 0)
        {
            return fail("set " + std::string(node) + " " + std::string(key) + " '" +
                        std::string(value) + "' failed (" + err_text(r) + ")");
        }
        return 0;
    }

    int cmd_set(std::string_view line, const std::vector<token> &t)
    {
        if (t.size() < 4)
        {
            return fail("usage: set <node> <key> <value...>");
        }
        const std::string_view value = trim(line.substr(t[3].offset));
        return configure(t[1].text, t[2].text, value);
    }

    /* Resolves one link end in the current pipeline. Queue ends add their endpoint node the
     * first time. */
    int resolve(std::string_view text, bool first, bool last, std::string *node, uint8_t *port)
    {
        std::string_view name = text;
        *port = 0;
        const size_t colon = text.rfind(':');
        if (std::string_view::npos != colon)
        {
            int64_t p = 0;
            if (key_parse_i64(text.substr(colon + 1), &p) != 0 || p < 0 || p > 255)
            {
                return fail("bad port in '" + std::string(text) + "'");
            }
            *port = static_cast<uint8_t>(p);
            name = text.substr(0, colon);
        }
        if (!name.empty() && '@' == name.front())
        {
            return resolve_queue(name, first, last, node);
        }
        const impl::node_rec *n = s_.find_node(name);
        if (nullptr == n)
        {
            return fail("unknown node '" + std::string(name) + "'");
        }
        if (n->pipe != cur_)
        {
            return fail("node '" + std::string(name) + "' is in pipeline '" + n->pipe->name +
                        "'; use a queue to cross pipelines");
        }
        *node = std::string(name);
        return 0;
    }

    int resolve_queue(std::string_view name, bool first, bool last, std::string *node)
    {
        impl::queue_entry *q = s_.find_queue(name.substr(1));
        if (nullptr == q)
        {
            return fail("unknown queue '" + std::string(name.substr(1)) + "'");
        }
        if (first == last)
        {
            return fail("queue '" + q->name + "' must start or end a link");
        }
        impl::pipe_entry *&end = first ? q->reader : q->writer;
        if (nullptr != end && end != cur_)
        {
            return fail("queue '" + q->name + "' already has a " +
                        (first ? "reader" : "writer") + " in pipeline '" + end->name + "'");
        }
        if ((first ? q->writer : q->reader) == cur_)
        {
            return fail("queue '" + q->name + "' would read and write in the same pipeline");
        }
        *node = std::string(name);
        if (nullptr == end)
        {
            const int r = first ? cur_->p->add_queue_source(*node, q->q)
                                : cur_->p->add_queue_sink(*node, q->q);
            if (r < 0)
            {
                return fail("cannot add queue end '" + *node + "' (" + err_text(r) + ")");
            }
            end = cur_;
            s_.nodes.push_back({cur_, *node, first ? "queue_source" : "queue_sink"});
        }
        return 0;
    }

    int cmd_link(const std::vector<token> &t)
    {
        if (nullptr == cur_)
        {
            return fail("link outside a pipeline");
        }
        if (t.size() < 4 || (t.size() % 2) != 0)
        {
            return fail("usage: link <a> -> <b> [-> <c> ...]");
        }
        for (size_t i = 2; i < t.size(); i += 2)
        {
            if ("->" != t[i].text)
            {
                return fail("expected '->', got '" + std::string(t[i].text) + "'");
            }
        }
        std::string prev;
        uint8_t     prev_port = 0;
        for (size_t i = 1; i < t.size(); i += 2)
        {
            std::string node;
            uint8_t     port = 0;
            if (resolve(t[i].text, 1 == i, t.size() - 1 == i, &node, &port) < 0)
            {
                return -EINVAL;
            }
            if (i > 1)
            {
                const int r = cur_->p->link(prev, prev_port, node, port);
                if (r < 0)
                {
                    std::string why = err_text(r);
                    if (-EINVAL == r)
                    {
                        why = "'" + prev + "' has no output or '" + node + "' has no input";
                    }
                    else if (-ELOOP == r)
                    {
                        why = "would make a cycle";
                    }
                    else if (-EEXIST == r)
                    {
                        why = "already linked";
                    }
                    return fail("cannot link " + prev + " -> " + node + ": " + why);
                }
            }
            prev = node;
            prev_port = port;
        }
        return 0;
    }

    int finish()
    {
        for (const auto &q : s_.queues)
        {
            if (nullptr == q.writer || nullptr == q.reader)
            {
                return fail(q.line, "queue '" + q.name + "' needs a writer and a reader");
            }
        }
        if (s_.pipes.empty())
        {
            return fail("no pipeline declared");
        }
        s_.compute_start_order();
        return 0;
    }

    impl                &s_;
    std::string_view     source_;
    int                  line_no_ = 0;
    impl::pipe_entry    *cur_ = nullptr;
    std::string          message_;
};

}  // namespace

pipeline_set::pipeline_set() : impl_(std::make_unique<impl>())
{
    impl_->factory = [this](std::string_view type) {
        created_node c;
        const std::string t(type);
        c.source = component_factory::create_source(t);
        if (!c.source)
        {
            c.coder = component_factory::create_coder(t);
        }
        if (!c.source && !c.coder)
        {
            c.sink = component_factory::create_sink(t);
            if (c.sink && type == "stream_sender")
            {
                if (!impl_->sender_reactor)
                {
                    impl_->sender_reactor = std::make_unique<default_reactor_host>();
                }
                c.sink->configure("reactor", impl_->sender_reactor->name());
            }
        }
        return c;
    };
}

pipeline_set::~pipeline_set()
{
    impl_->clear();
}

void pipeline_set::set_node_factory(node_factory_fn fn)
{
    impl_->factory = std::move(fn);
}

int pipeline_set::load_file(const std::string &path, std::string *err)
{
    std::ifstream in(path);
    if (!in)
    {
        *err = path + ": cannot open (" + std::strerror(errno) + ")";
        return -ENOENT;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return load_text(ss.str(), path, err);
}

int pipeline_set::load_text(std::string_view text, std::string_view source_name, std::string *err)
{
    if (impl_->loaded)
    {
        *err = "already loaded";
        return -EALREADY;
    }
    loader    l(*impl_, source_name);
    const int r = l.run(text, err);
    if (r < 0)
    {
        impl_->clear();
        return r;
    }
    impl_->loaded = true;
    return 0;
}

int pipeline_set::configure(std::string_view node, std::string_view key, std::string_view value)
{
    const impl::node_rec *n = impl_->find_node(node);
    return (nullptr != n) ? n->pipe->p->configure(node, key, value) : -ENOENT;
}

int pipeline_set::query(std::string_view node, std::string_view key, std::string *value) const
{
    const impl::node_rec *n = impl_->find_node(node);
    return (nullptr != n) ? n->pipe->p->query(node, key, value) : -ENOENT;
}

std::vector<pipeline_set::node_info> pipeline_set::nodes() const
{
    std::vector<node_info> out;
    for (const auto &n : impl_->nodes)
    {
        out.push_back({n.pipe->name, n.name, n.type});
    }
    return out;
}

std::vector<std::pair<std::string, pipeline_state_e>> pipeline_set::states() const
{
    std::vector<std::pair<std::string, pipeline_state_e>> out;
    for (const auto &p : impl_->pipes)
    {
        out.emplace_back(p->name, p->p->state());
    }
    return out;
}

int pipeline_set::start(std::string_view target)
{
    return impl_->control(target, want_e::RUNNING);
}

int pipeline_set::pause(std::string_view target)
{
    return impl_->control(target, want_e::PAUSED);
}

int pipeline_set::stop(std::string_view target)
{
    return impl_->control(target, want_e::STOPPED);
}

void pipeline_set::wake_supervisor()
{
    impl_->fail_cv.notify_all();
}

uint64_t pipeline_set::restarts() const
{
    return impl_->restart_count.load();
}

void pipeline_set::supervise(const std::atomic<bool> &run, std::chrono::milliseconds backoff)
{
    impl &s = *impl_;
    while (run.load())
    {
        std::string pipe;
        std::string node;
        int         err = 0;
        {
            std::unique_lock<std::mutex> lock(s.fail_mu);
            s.fail_cv.wait_for(lock, std::chrono::milliseconds(200),
                               [&] { return s.failure_pending || !run.load(); });
            if (!run.load() || !s.failure_pending)
            {
                continue;
            }
            pipe = s.fail_pipe;
            node = s.fail_node;
            err = s.fail_err;
        }
        std::fprintf(stderr, "vstreamer: %s/%s failed (%d %s), restarting in %lld ms\n",
                     pipe.c_str(), node.c_str(), err, errno_name(err),
                     static_cast<long long>(backoff.count()));

        {
            std::lock_guard<std::mutex> lock(s.ctl_mu);
            for (auto it = s.start_order.rbegin(); it != s.start_order.rend(); ++it)
            {
                (void)(*it)->p->stop();
            }
        }
        {
            /* Failures from before the stop are covered by this restart. */
            std::unique_lock<std::mutex> lock(s.fail_mu);
            s.failure_pending = false;
            s.fail_cv.wait_for(lock, backoff, [&] { return !run.load(); });
        }
        if (!run.load())
        {
            break;
        }

        std::lock_guard<std::mutex> lock(s.ctl_mu);
        for (impl::pipe_entry *p : s.start_order)
        {
            if (want_e::STOPPED == p->want)
            {
                continue;
            }
            const int r = (want_e::RUNNING == p->want) ? p->p->start() : p->p->pause();
            if (r < 0)
            {
                std::string failed_node;
                int         failed_err = 0;
                p->p->last_error(&failed_node, &failed_err);
                s.on_pipeline_error(p->name, failed_node, r);
                break;
            }
        }
        s.restart_count.fetch_add(1);
    }
}

}  // namespace vstreamer::app
