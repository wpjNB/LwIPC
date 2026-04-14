// loaned_sample.cpp
//
// LwIPC loaned sample — implementation unit.
//
// The LoanedSample class template is fully defined in the header
// (include/lwipc/loaned_sample.hpp).  This compilation unit exists to:
//   1. Confirm the header compiles without external dependencies.
//   2. Serve as a future home for any non-template RAII helpers or
//      debug instrumentation that tracks outstanding loans.
//
// TODO(M2): Add optional compile-time instrumentation that counts outstanding
//           loans via a thread_local counter for leak detection in debug builds.

#include "lwipc/loaned_sample.hpp"
