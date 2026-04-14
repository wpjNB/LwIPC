// subscriber.cpp
//
// LwIPC subscriber — implementation unit.
//
// Subscriber<T, Capacity> and IntraSubscriber<T, Capacity> are fully defined
// in include/lwipc/subscriber.hpp.  This compilation unit exists to:
//   1. Verify the header compiles cleanly as a standalone translation unit.
//   2. Provide a home for future non-template subscriber utilities such as
//      executor integration and priority-aware callback dispatching.
//
// TODO(M2): Integrate with a static Executor that manages subscriber threads
//           with configurable CPU affinity and SCHED_FIFO priority.
// TODO(M2): Add history (keep_last N) and QoS filter (deadline / lifespan).
// TODO(M3): Support slow-consumer detection and configurable eviction policy.

#include "lwipc/subscriber.hpp"
