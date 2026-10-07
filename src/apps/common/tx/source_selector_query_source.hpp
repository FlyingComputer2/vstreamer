#ifndef VSTREAMER_APPS_TX_SOURCE_SELECTOR_QUERY_SOURCE_HPP
#define VSTREAMER_APPS_TX_SOURCE_SELECTOR_QUERY_SOURCE_HPP

#include "apps/common/tx/source_selector.hpp"
#include "core/component_source.hpp"

namespace vstreamer::apps::tx
{

/* component_source for metrics/console query only (output is via source_selector::poll_once). */
class source_selector_query_source : public component_source
{
public:
    explicit source_selector_query_source(source_selector &selector_in) : selector(selector_in) {}

    [[nodiscard]] std::string name() const override
    {
        return "source_selector";
    }

    int open() override
    {
        return 0;
    }

    void close() override {}

    int output(component_pdu & /*out*/) override
    {
        return -ENOTSUP;
    }

    int configure(std::string_view key, std::string_view value) override
    {
        return selector.configure(key, value);
    }

    int query(std::string_view key, std::string *value) const override
    {
        return selector.query(key, value);
    }

private:
    source_selector &selector;
};

}  // namespace vstreamer::apps::tx

#endif  // VSTREAMER_APPS_TX_SOURCE_SELECTOR_QUERY_SOURCE_HPP
