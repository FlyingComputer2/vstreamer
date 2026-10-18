#ifndef VSTREAMER_COMPONENTS_STREAM_SENDER_PACE_HPP
#define VSTREAMER_COMPONENTS_STREAM_SENDER_PACE_HPP

#include <algorithm>

namespace vstreamer::detail
{

/*
 * stream_sender's choice of the next datagram to send. The queue is sorted by release time.
 * A spread FEC shard (gap > 0) is due at its release time and no sooner than `gap` after its
 * block's previous shard left (`last[block]`), so a send thread that fell behind keeps the pace
 * within a block instead of flushing it in a lump, and sends other due datagrams meanwhile.
 *
 * Returns the first due entry at time t, or q.end(); *until is then the earliest time one can
 * become due (capped by its input value).
 */
template <class Queue, class LastMap, class TimePoint>
typename Queue::iterator pick_due(Queue &q, const LastMap &last, TimePoint t, TimePoint *until)
{
    for (auto it = q.begin(); it != q.end(); ++it)
    {
        if (it->release > t)
        {
            *until = std::min(*until, it->release);
            break;
        }
        auto due = it->release;
        if (it->gap.count() > 0)
        {
            const auto prev = last.find(it->block);
            if (prev != last.end())
            {
                due = std::max(due, prev->second + it->gap);
            }
        }
        if (due <= t)
        {
            return it;
        }
        *until = std::min(*until, due);
    }
    return q.end();
}

}  // namespace vstreamer::detail

#endif  // VSTREAMER_COMPONENTS_STREAM_SENDER_PACE_HPP
