#include "core/pipeline.hpp"

#include <cerrno>
#include <climits>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "core/component.hpp"
#include "core/component_coder.hpp"
#include "core/component_factory.hpp"
#include "core/component_input.hpp"
#include "core/component_output.hpp"
#include "core/component_sink.hpp"
#include "core/component_source.hpp"
#include "core/pdu_wakeup.hpp"
#include "core/pipeline_queue.hpp"
#include "core/thread_affinity.hpp"
#include "core/time_util.hpp"

namespace vstreamer
{
namespace
{

/* Upper bound on one idle sleep, so a node that never notifies and has no deadline is still
 * polled. */
constexpr int64_t k_idle_wait_cap_ns = 50'000'000LL;
/* Retry interval for a PDU held on a link because the next node returned -EAGAIN (the stage
 * threads in apps/ retry after 200 us). */
constexpr int64_t k_held_retry_ns = 200'000LL;
/* PDUs pulled from one node per pass before moving on, so one busy node cannot starve the
 * others or delay a control command. */
constexpr int k_max_pulls_per_pass = 64;

constexpr std::string_view k_rejects_key = "pipeline.rejects";

enum class command_e : uint8_t
{
    NONE,
    RUN,
    PAUSE,
    STOP,
};

[[nodiscard]] bool is_pdu_reject(int err)
{
    return -EINVAL == err || -ENOTSUP == err;
}

}  // namespace

const char *pipeline_state_name(pipeline_state_e s)
{
    switch (s)
    {
        case pipeline_state_e::STOPPED:
            return "stopped";
        case pipeline_state_e::PAUSED:
            return "paused";
        case pipeline_state_e::RUNNING:
            return "running";
        case pipeline_state_e::ERROR:
            return "error";
    }
    return "stopped";
}

struct pipeline::impl
{
    struct node_entry;

    struct link_entry
    {
        node_entry                  *from = nullptr;
        uint8_t                      from_port = 0;
        node_entry                  *to = nullptr;
        uint8_t                      to_port = 0;
        std::optional<component_pdu> held;
    };

    struct node_entry
    {
        std::string                name;
        std::unique_ptr<component> owner;
        component_input           *in = nullptr;
        component_output          *out = nullptr;
        bool                       is_source = false;
        std::function<int()>       open_fn;
        std::function<void()>      close_fn;
        std::vector<link_entry *>  out_links;
        std::atomic<uint64_t>      rejects {0};
        bool                       opened = false;
    };

    explicit impl(std::string n) : name(std::move(n)), wake(std::make_shared<pdu_wakeup>()) {}

    [[nodiscard]] node_entry *find(std::string_view node_name) const
    {
        for (const auto &n : nodes)
        {
            if (n->name == node_name)
            {
                return n.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool reaches(const node_entry *from, const node_entry *target) const;
    void               rebuild_order();
    int                add_node(std::string node_name, std::unique_ptr<component> owner,
                                component_input *in, component_output *out, bool is_source,
                                std::function<int()> open_fn, std::function<void()> close_fn);

    int  open_all();
    void close_all();
    void thread_main(std::promise<int> started, command_e first);
    bool step();
    bool flush_held(node_entry &n);
    bool deliver(node_entry &n, component_pdu &&pdu);
    void fail(const node_entry &n, int err);
    int  post(command_e cmd);
    [[nodiscard]] int64_t next_deadline() const;

    const std::string name;

    /* Guards nodes/links against configure()/query() lookups while the graph is built. The
     * graph is immutable outside STOPPED, so the pipeline thread reads it without this lock. */
    mutable std::mutex                       graph_mu;
    std::vector<std::unique_ptr<node_entry>> nodes;
    std::vector<std::unique_ptr<link_entry>> links;
    /* Sources first; every link goes forward in this order. */
    std::vector<node_entry *> order;

    /* Serializes build and control calls. */
    std::mutex ctl_mu;

    std::shared_ptr<pdu_wakeup>   wake;
    std::thread                   thread;
    std::atomic<std::thread::id>  thread_id {};
    std::atomic<pipeline_state_e> state {pipeline_state_e::STOPPED};
    int                           cpu = -1;

    /* Command mailbox to the pipeline thread. */
    std::mutex              cmd_mu;
    std::condition_variable cmd_cv;
    command_e               cmd = command_e::NONE;
    bool                    cmd_done = false;
    int                     cmd_result = 0;

    mutable std::mutex err_mu;
    std::string        err_node;
    int                err_code = 0;
    error_fn           on_error;
};

bool pipeline::impl::reaches(const node_entry *from, const node_entry *target) const
{
    if (from == target)
    {
        return true;
    }
    for (const link_entry *l : from->out_links)
    {
        if (reaches(l->to, target))
        {
            return true;
        }
    }
    return false;
}

/* Kahn's algorithm; ties keep insertion order so the result is stable. */
void pipeline::impl::rebuild_order()
{
    std::vector<int> indegree(nodes.size(), 0);
    auto index_of = [this](const node_entry *n) {
        for (size_t i = 0; i < nodes.size(); i++)
        {
            if (nodes[i].get() == n)
            {
                return i;
            }
        }
        return nodes.size();
    };
    for (const auto &l : links)
    {
        indegree[index_of(l->to)]++;
    }
    order.clear();
    std::vector<bool> placed(nodes.size(), false);
    while (order.size() < nodes.size())
    {
        bool progress = false;
        for (size_t i = 0; i < nodes.size(); i++)
        {
            if (placed[i] || indegree[i] != 0)
            {
                continue;
            }
            placed[i] = true;
            progress = true;
            order.push_back(nodes[i].get());
            for (const link_entry *l : nodes[i]->out_links)
            {
                indegree[index_of(l->to)]--;
            }
        }
        if (!progress)
        {
            break; /* cycle; link() refuses those, so not reached */
        }
    }
}

int pipeline::impl::add_node(std::string node_name, std::unique_ptr<component> owner,
                             component_input *in, component_output *out, bool is_source,
                             std::function<int()> open_fn, std::function<void()> close_fn)
{
    if (node_name.empty() || !owner)
    {
        return -EINVAL;
    }
    std::lock_guard<std::mutex> ctl(ctl_mu);
    if (state.load() != pipeline_state_e::STOPPED)
    {
        return -EBUSY;
    }
    std::lock_guard<std::mutex> lock(graph_mu);
    if (nullptr != find(node_name))
    {
        return -EEXIST;
    }
    auto n = std::make_unique<node_entry>();
    n->name = std::move(node_name);
    n->in = in;
    n->out = out;
    n->is_source = is_source;
    n->open_fn = std::move(open_fn);
    n->close_fn = std::move(close_fn);
    owner->set_wakeup(wake);
    n->owner = std::move(owner);
    nodes.push_back(std::move(n));
    rebuild_order();
    return 0;
}

/* Sinks first, sources last, so nothing produces into a closed node. */
int pipeline::impl::open_all()
{
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        const int r = (*it)->open_fn();
        if (r < 0)
        {
            close_all();
            std::lock_guard<std::mutex> lock(err_mu);
            err_node = (*it)->name;
            err_code = r;
            return r;
        }
        (*it)->opened = true;
    }
    return 0;
}

/* Sources first. */
void pipeline::impl::close_all()
{
    for (const auto &l : links)
    {
        l->held.reset();
    }
    for (node_entry *n : order)
    {
        if (n->opened)
        {
            n->close_fn();
            n->opened = false;
        }
    }
}

void pipeline::impl::fail(const node_entry &n, int err)
{
    state.store(pipeline_state_e::ERROR);
    error_fn fn;
    {
        std::lock_guard<std::mutex> lock(err_mu);
        err_node = n.name;
        err_code = err;
        fn = on_error;
    }
    if (fn)
    {
        fn(n.name, err);
    }
}

/* Retries the PDUs this node's links hold. True when all its links are empty. */
bool pipeline::impl::flush_held(node_entry &n)
{
    bool clear = true;
    for (link_entry *l : n.out_links)
    {
        if (!l->held)
        {
            continue;
        }
        component_pdu attempt = *l->held;
        const int     r = l->to->in->input(std::move(attempt));
        if (-EAGAIN == r)
        {
            clear = false;
            continue;
        }
        l->held.reset();
        if (0 == r)
        {
            continue;
        }
        if (is_pdu_reject(r))
        {
            l->to->rejects.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        fail(*l->to, r);
        return false;
    }
    return clear;
}

/* Hands one PDU to every link on its port. False when a link now holds it (stop pulling this
 * node) or a downstream node failed. */
bool pipeline::impl::deliver(node_entry &n, component_pdu &&pdu)
{
    const uint8_t port = pdu.port;
    bool          clear = true;
    for (link_entry *l : n.out_links)
    {
        if (l->from_port != port)
        {
            continue;
        }
        /* input() may consume its argument even when it refuses, so the original stays here and
         * is copied again only on -EAGAIN. */
        component_pdu attempt = pdu;
        attempt.port = l->to_port;
        const int r = l->to->in->input(std::move(attempt));
        if (0 == r)
        {
            continue;
        }
        if (-EAGAIN == r)
        {
            l->held = pdu;
            l->held->port = l->to_port;
            clear = false;
            continue;
        }
        if (is_pdu_reject(r))
        {
            l->to->rejects.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        fail(*l->to, r);
        return false;
    }
    return clear;
}

/* One pass over the graph in source-first order, so a PDU can cross the whole pipeline in one
 * pass. True when any PDU moved. */
bool pipeline::impl::step()
{
    bool moved = false;
    for (node_entry *n : order)
    {
        if (nullptr == n->out)
        {
            continue;
        }
        if (!flush_held(*n))
        {
            if (state.load() == pipeline_state_e::ERROR)
            {
                return moved;
            }
            continue;
        }
        for (int i = 0; i < k_max_pulls_per_pass; i++)
        {
            component_pdu pdu;
            const int     r = n->out->output(pdu);
            if (-EAGAIN == r || (-ENODEV == r && n->is_source))
            {
                break;
            }
            if (r < 0)
            {
                fail(*n, r);
                return moved;
            }
            moved = true;
            if (!deliver(*n, std::move(pdu)))
            {
                if (state.load() == pipeline_state_e::ERROR)
                {
                    return moved;
                }
                break;
            }
        }
    }
    return moved;
}

/* Earliest node deadline, capped. A node whose output link is held is not pulled, so its
 * deadline is skipped (waiting on a past one would spin); the held link is retried after
 * k_held_retry_ns instead, since nodes notify when output is ready, not when input room frees
 * up. Any other node whose deadline has passed (it fell due during the pass) gets another pass
 * after k_held_retry_ns rather than being missed until the cap. */
int64_t pipeline::impl::next_deadline() const
{
    const int64_t now = steady_mono_ns();
    int64_t       deadline = now + k_idle_wait_cap_ns;
    for (const node_entry *n : order)
    {
        const bool held = std::any_of(n->out_links.begin(), n->out_links.end(),
                                      [](const link_entry *l) { return l->held.has_value(); });
        if (held)
        {
            deadline = std::min(deadline, now + k_held_retry_ns);
            continue;
        }
        const int64_t d = n->owner->next_deadline_ns();
        deadline = std::min(deadline, std::max(d, now + k_held_retry_ns));
    }
    return deadline;
}

void pipeline::impl::thread_main(std::promise<int> started, command_e first)
{
    thread_id.store(std::this_thread::get_id());
    pin_current_thread_to_cpu(cpu);

    const int r = open_all();
    if (r < 0)
    {
        started.set_value(r);
        return;
    }
    state.store(command_e::RUN == first ? pipeline_state_e::RUNNING : pipeline_state_e::PAUSED);
    started.set_value(0);

    for (;;)
    {
        command_e c = command_e::NONE;
        {
            std::lock_guard<std::mutex> lock(cmd_mu);
            c = cmd;
        }
        if (command_e::NONE != c)
        {
            int result = 0;
            const pipeline_state_e s = state.load();
            if (command_e::STOP == c)
            {
                close_all();
                state.store(pipeline_state_e::STOPPED);
            }
            else if (pipeline_state_e::ERROR == s)
            {
                result = -EIO;
            }
            else
            {
                state.store(command_e::RUN == c ? pipeline_state_e::RUNNING
                                                : pipeline_state_e::PAUSED);
            }
            {
                std::lock_guard<std::mutex> lock(cmd_mu);
                cmd = command_e::NONE;
                cmd_result = result;
                cmd_done = true;
            }
            cmd_cv.notify_all();
            if (command_e::STOP == c)
            {
                return;
            }
        }

        const bool moved = pipeline_state_e::RUNNING == state.load() && step();
        if (!moved)
        {
            wake->wait_until(pipeline_state_e::RUNNING == state.load()
                                 ? next_deadline()
                                 : steady_mono_ns() + k_idle_wait_cap_ns);
        }
    }
}

/* Runs cmd on the pipeline thread and waits for its result. Caller holds ctl_mu. */
int pipeline::impl::post(command_e c)
{
    std::unique_lock<std::mutex> lock(cmd_mu);
    cmd = c;
    cmd_done = false;
    lock.unlock();
    wake->notify();
    lock.lock();
    cmd_cv.wait(lock, [this] { return cmd_done; });
    return cmd_result;
}

pipeline::pipeline(std::string name) : impl_(std::make_unique<impl>(std::move(name))) {}

pipeline::~pipeline()
{
    (void)stop();
}

const std::string &pipeline::name() const
{
    return impl_->name;
}

int pipeline::add_source(std::string node_name, std::string_view factory_type)
{
    auto c = component_factory::create_source(std::string(factory_type));
    if (!c)
    {
        return -ENODEV;
    }
    return add_source(std::move(node_name), std::move(c));
}

int pipeline::add_coder(std::string node_name, std::string_view factory_type)
{
    auto c = component_factory::create_coder(std::string(factory_type));
    if (!c)
    {
        return -ENODEV;
    }
    return add_coder(std::move(node_name), std::move(c));
}

int pipeline::add_sink(std::string node_name, std::string_view factory_type)
{
    auto c = component_factory::create_sink(std::string(factory_type));
    if (!c)
    {
        return -ENODEV;
    }
    return add_sink(std::move(node_name), std::move(c));
}

int pipeline::add_source(std::string node_name, std::unique_ptr<component_source> node)
{
    if (!node)
    {
        return -EINVAL;
    }
    component_source *p = node.get();
    return impl_->add_node(std::move(node_name), std::unique_ptr<component>(node.release()),
                           nullptr, p, true, [p] { return p->open(); }, [p] { p->close(); });
}

int pipeline::add_coder(std::string node_name, std::unique_ptr<component_coder> node)
{
    if (!node)
    {
        return -EINVAL;
    }
    component_coder *p = node.get();
    return impl_->add_node(std::move(node_name), std::unique_ptr<component>(node.release()), p, p,
                           false, [p] { return p->open(); }, [p] { p->close(); });
}

int pipeline::add_sink(std::string node_name, std::unique_ptr<component_sink> node)
{
    if (!node)
    {
        return -EINVAL;
    }
    component_sink *p = node.get();
    return impl_->add_node(std::move(node_name), std::unique_ptr<component>(node.release()), p,
                           nullptr, false, [p] { return p->open(); }, [p] { p->close(); });
}

int pipeline::add_queue_sink(std::string node_name, std::shared_ptr<pipeline_queue> q)
{
    if (!q)
    {
        return -EINVAL;
    }
    return add_sink(std::move(node_name), make_pipeline_queue_sink(std::move(q)));
}

int pipeline::add_queue_source(std::string node_name, std::shared_ptr<pipeline_queue> q)
{
    if (!q)
    {
        return -EINVAL;
    }
    const int r = add_source(std::move(node_name), make_pipeline_queue_source(q));
    if (0 == r)
    {
        q->bind_consumer_wakeup(impl_->wake);
    }
    return r;
}

int pipeline::link(std::string_view from, std::string_view to)
{
    return link(from, 0, to, 0);
}

int pipeline::link(std::string_view from, uint8_t from_port, std::string_view to, uint8_t to_port)
{
    std::lock_guard<std::mutex> ctl(impl_->ctl_mu);
    if (impl_->state.load() != pipeline_state_e::STOPPED)
    {
        return -EBUSY;
    }
    std::lock_guard<std::mutex> lock(impl_->graph_mu);
    impl::node_entry *a = impl_->find(from);
    impl::node_entry *b = impl_->find(to);
    if (nullptr == a || nullptr == b)
    {
        return -ENOENT;
    }
    if (nullptr == a->out || nullptr == b->in)
    {
        return -EINVAL;
    }
    for (const impl::link_entry *l : a->out_links)
    {
        if (l->to == b && l->from_port == from_port && l->to_port == to_port)
        {
            return -EEXIST;
        }
    }
    if (impl_->reaches(b, a))
    {
        return -ELOOP;
    }
    auto l = std::make_unique<impl::link_entry>();
    l->from = a;
    l->from_port = from_port;
    l->to = b;
    l->to_port = to_port;
    a->out_links.push_back(l.get());
    impl_->links.push_back(std::move(l));
    impl_->rebuild_order();
    return 0;
}

int pipeline::link_chain(const std::vector<std::string_view> &nodes)
{
    for (size_t i = 1; i < nodes.size(); i++)
    {
        const int r = link(nodes[i - 1], nodes[i]);
        if (r < 0)
        {
            return r;
        }
    }
    return 0;
}

int pipeline::configure(std::string_view node_name, std::string_view key, std::string_view value)
{
    component *c = node(node_name);
    if (nullptr == c)
    {
        return -ENOENT;
    }
    return c->configure(key, value);
}

int pipeline::query(std::string_view node_name, std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    std::unique_lock<std::mutex> lock(impl_->graph_mu);
    impl::node_entry *n = impl_->find(node_name);
    if (nullptr == n)
    {
        return -ENOENT;
    }
    if (k_rejects_key == key)
    {
        *value = std::to_string(n->rejects.load(std::memory_order_relaxed));
        return 0;
    }
    component *c = n->owner.get();
    lock.unlock();
    return c->query(key, value);
}

std::vector<std::string> pipeline::node_names() const
{
    std::lock_guard<std::mutex> lock(impl_->graph_mu);
    std::vector<std::string> names;
    names.reserve(impl_->nodes.size());
    for (const auto &n : impl_->nodes)
    {
        names.push_back(n->name);
    }
    return names;
}

component *pipeline::node(std::string_view node_name) const
{
    std::lock_guard<std::mutex> lock(impl_->graph_mu);
    impl::node_entry *n = impl_->find(node_name);
    return (nullptr != n) ? n->owner.get() : nullptr;
}

int pipeline::start()
{
    if (std::this_thread::get_id() == impl_->thread_id.load())
    {
        return -EDEADLK;
    }
    std::lock_guard<std::mutex> ctl(impl_->ctl_mu);
    if (impl_->state.load() != pipeline_state_e::STOPPED)
    {
        return impl_->post(command_e::RUN);
    }
    return spawn(true);
}

int pipeline::pause()
{
    if (std::this_thread::get_id() == impl_->thread_id.load())
    {
        return -EDEADLK;
    }
    std::lock_guard<std::mutex> ctl(impl_->ctl_mu);
    if (impl_->state.load() != pipeline_state_e::STOPPED)
    {
        return impl_->post(command_e::PAUSE);
    }
    return spawn(false);
}

int pipeline::stop()
{
    if (std::this_thread::get_id() == impl_->thread_id.load())
    {
        return -EDEADLK;
    }
    std::lock_guard<std::mutex> ctl(impl_->ctl_mu);
    if (impl_->state.load() == pipeline_state_e::STOPPED)
    {
        return 0;
    }
    const int r = impl_->post(command_e::STOP);
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
    impl_->thread_id.store(std::thread::id());
    return r;
}

/* Caller holds ctl_mu and the pipeline is STOPPED. */
int pipeline::spawn(bool run)
{
    {
        std::lock_guard<std::mutex> lock(impl_->err_mu);
        impl_->err_node.clear();
        impl_->err_code = 0;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->cmd_mu);
        impl_->cmd = command_e::NONE;
    }
    std::promise<int> started;
    std::future<int>  result = started.get_future();
    impl_->thread = std::thread(&impl::thread_main, impl_.get(), std::move(started),
                                run ? command_e::RUN : command_e::PAUSE);
    const int r = result.get();
    if (r < 0)
    {
        impl_->thread.join();
        impl_->thread_id.store(std::thread::id());
    }
    return r;
}

pipeline_state_e pipeline::state() const
{
    return impl_->state.load();
}

void pipeline::last_error(std::string *node_name, int *err) const
{
    std::lock_guard<std::mutex> lock(impl_->err_mu);
    if (nullptr != node_name)
    {
        *node_name = impl_->err_node;
    }
    if (nullptr != err)
    {
        *err = impl_->err_code;
    }
}

int pipeline::set_cpu(int cpu)
{
    std::lock_guard<std::mutex> ctl(impl_->ctl_mu);
    if (impl_->state.load() != pipeline_state_e::STOPPED)
    {
        return -EBUSY;
    }
    impl_->cpu = cpu;
    return 0;
}

void pipeline::set_error_handler(error_fn fn)
{
    std::lock_guard<std::mutex> lock(impl_->err_mu);
    impl_->on_error = std::move(fn);
}

}  // namespace vstreamer
