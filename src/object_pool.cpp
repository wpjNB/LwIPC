// object_pool.cpp
//
// LwIPC object pool — implementation unit.
//
// The ObjectPool class template is fully defined in the header
// (include/lwipc/object_pool.hpp).  This compilation unit exists to:
//   1. Verify that the header compiles cleanly as a standalone translation unit.
//   2. Provide a home for any future non-template helper code (e.g., diagnostic
//      utilities, pool statistics aggregation) without polluting the header.
//
// TODO(M2): Add pool-level statistics (peak usage, alloc failures) and expose
//           them via a lightweight metrics interface.
// TODO(M3): Add NUMA-aware pool variant that pre-faults pages on a specific
//           NUMA node for real-time workloads.

#include "lwipc/object_pool.hpp"
