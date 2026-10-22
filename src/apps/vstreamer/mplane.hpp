#ifndef VSTREAMER_APPS_VSTREAMER_MPLANE_HPP
#define VSTREAMER_APPS_VSTREAMER_MPLANE_HPP

#include <string>

namespace vstreamer::app
{

class pipeline_set;

/*
 * Management plane: one request per UDP datagram (served by apps::app_console), replies end in
 * '\n'. Success is "ok" or "ok <value>"; failure is "err <errno> <NAME>", e.g.
 * "err -2 ENOENT". Listing verbs answer one "ok ..." line per item.
 *
 *   get <node> <key>              query
 *   set <node> <key> <value...>   configure (the rest of the line is the value)
 *   nodes                         "ok <pipeline> <node> <type>" per node
 *   state                         "ok <pipeline> <state>" per pipeline
 *   start|pause|stop [<pipeline>|all]
 */
[[nodiscard]] std::string mplane_help();

/* Returns false when line is not an mplane verb, so app_console's built-ins (ping, help) run. */
bool handle_mplane_line(pipeline_set &set, const char *line, std::string &reply);

}  // namespace vstreamer::app

#endif  // VSTREAMER_APPS_VSTREAMER_MPLANE_HPP
