#ifndef Q2_INTERNAL_H
#define Q2_INTERNAL_H
#include "q2/kernel.h"
#include "q2/log.h"
struct q2_kernel {
    q2_port_t port;
    const q2_component_t *components[Q2_COMPONENT_MAX];
    q2_state_t states[Q2_COMPONENT_MAX];
    q2_result_t errors[Q2_COMPONENT_MAX];
    size_t count, order[Q2_COMPONENT_MAX], ordered;
    bool booted, running, polling;
    q2_event_t events[Q2_EVENT_MAX];
    size_t head, used;
    uint64_t posted, dropped, next_instance;
    q2_runtime_driver_t runtimes[Q2_RUNTIME_MAX];
    size_t attached;
    q2_result_t component_poll_result;
#if Q2_ENABLE_LOG
    q2_log_sink_t log_sink;
    q2_log_level_t log_level;
    bool logging;
#endif
};
#endif
