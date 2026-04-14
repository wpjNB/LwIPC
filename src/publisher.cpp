// publisher.cpp
//
// LwIPC publisher — implementation unit.
//
// Publisher<T, Capacity> and IntraPublisher<T, Capacity> are fully defined in
// include/lwipc/publisher.hpp.  This compilation unit exists to:
//   1. Verify the header compiles cleanly as a standalone translation unit.
//   2. Provide a home for future non-template publisher utilities such as
//      channel registry, lifecycle hooks, and QoS enforcement logic.
//
// TODO(M2): Add a ChannelRegistry (singleton-free, passed by reference) that
//           maps channel names to live publisher endpoints for discovery.
// TODO(M2): Add deadline/lifespan QoS enforcement using a timer wheel.
// TODO(M3): Add backpressure signalling when the pool / ring is near capacity.

#include "lwipc/publisher.hpp"
