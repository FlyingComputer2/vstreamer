#include "apps/stream_sdl_test/channel_controller.hpp"

namespace vstreamer::test_app
{

channel_controller::channel_controller() = default;

channel_controller::~channel_controller()
{
    stop();
}

void channel_controller::set_bind_host(const char *host)
{
    link_.set_bind_host(host);
    console_.set_bind_host(host);
}

int channel_controller::start(int ingress_port, const char *egress_host, int egress_port)
{
    return link_.start(ingress_port, egress_host, egress_port);
}

int channel_controller::start_console(int console_port)
{
    return console_.start(link_, console_port);
}

void channel_controller::stop()
{
    console_.stop();
    link_.stop();
}

void channel_controller::set_pipeline_metrics(const vstreamer::metrics *source)
{
    console_.set_pipeline_metrics(source);
}

void channel_controller::set_pipeline_metrics_refresh(std::function<void()> refresh)
{
    console_.set_pipeline_metrics_refresh(std::move(refresh));
}

void channel_controller::set_pipeline_metrics_sync_live(std::function<void()> sync_live)
{
    console_.set_pipeline_metrics_sync_live(std::move(sync_live));
}

void channel_controller::set_source_state_metrics_refresh(std::function<void()> refresh)
{
    console_.set_source_state_metrics_refresh(std::move(refresh));
}

void channel_controller::set_encode_target(vstreamer::component_coder *encoder)
{
    console_.set_encode_target(encoder);
}

void channel_controller::set_encode_command_handlers(std::function<bool(int kbps)> set_cbr_kbps,
                                                     std::function<bool(int qp)> set_qp,
                                                     std::function<bool(int gop)> set_gop,
                                                     std::function<bool()> force_idr)
{
    console_.set_encode_command_handlers(std::move(set_cbr_kbps), std::move(set_qp),
                                         std::move(set_gop), std::move(force_idr));
}

void channel_controller::set_stream_sender(vstreamer::stream_sender *sender)
{
    console_.set_stream_sender(sender);
}

void channel_controller::set_max_kbps(double kbps)
{
    link_.set_max_kbps(kbps);
}

void channel_controller::set_drop_dt_ms(int ms)
{
    link_.set_drop_dt_ms(ms);
}

void channel_controller::set_constant_loss(double pct)
{
    link_.set_constant_loss(pct);
}

void channel_controller::set_queue_depth(int depth)
{
    link_.set_queue_depth(depth);
}

double channel_controller::max_kbps() const
{
    return link_.max_kbps();
}

int channel_controller::drop_dt_ms() const
{
    return link_.drop_dt_ms();
}

double channel_controller::constant_loss() const
{
    return link_.constant_loss();
}

int channel_controller::queue_depth() const
{
    return link_.queue_depth();
}

size_t channel_controller::forward_queue_size() const
{
    return link_.forward_queue_size();
}

channel_controller::forward_stats channel_controller::forward_stats_snapshot() const
{
    return link_.forward_stats_snapshot();
}

const std::atomic<uint64_t> &channel_controller::forward_bytes_out_counter() const
{
    return link_.forward_bytes_out_counter();
}

}  // namespace vstreamer::test_app
