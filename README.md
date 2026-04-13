# LwIPC — Lightweight IPC for Autonomous Driving

A header-only C++17 IPC framework using **POSIX shared memory** + **lock-free ring buffer** for high-performance inter-process communication between autonomous driving modules (sensor acquisition, AI inference, motion control).

---

## Features

| Feature | Detail |
|---|---|
| **Transport** | POSIX `shm_open` / `mmap` (local) + TCP socket (cross-host) |
| **Data structure** | Lock-free, SPMC ring buffer |
| **Sync primitives** | `std::atomic` with configurable memory ordering |
| **API style** | C++17, templates, RAII |
| **Messages** | `PointCloud`, `Image`, `IMU`, `Tensor`, `ControlCmd` |
| **Latency** | ~150 ns (same process, measured) |
| **Throughput** | > 90 GB/s (same process, measured) |

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    LwIPC Modules                        │
│                                                         │
│  IPCMessage      structured header + payload + CRC-32  │
│  SyncPolicy      memory_order encapsulation             │
│  ShmRingBuffer   lock-free SPMC ring backed by shm     │
│  Publisher       RAII write API (creates channel)       │
│  Subscriber      RAII read API (attaches to channel)    │
│  SocketTransport TCP framing for cross-host delivery    │
└─────────────────────────────────────────────────────────┘
```

### ShmRingBuffer — lock-free SPMC design

```
Shared memory layout:
  [ ControlBlock (64 B) | Slot[0] … Slot[N-1] ]

ControlBlock: atomic<uint64_t> write_pos

Each Slot:
  atomic<uint64_t> sequence   (even=empty/writing, odd=ready)
  T                data        (payload, zero-copy)
  uint8_t          _pad[…]    (cache-line align)

Producer: fetch_add(write_pos) → sequence(even) → copy → sequence(odd)
Consumer: check sequence == expected_odd → copy → ++cursor
```

---

## Quick Start

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure     # run all tests
```

### Publish (producer process)

```cpp
#include "lwipc/lwipc.hpp"
using namespace lwipc;

Publisher<PointXYZI, 128> pub("/lidar", MsgType::POINTCLOUD, /*sensor_id=*/1);
PointXYZI pt{1.f, 2.f, 3.f, 0.8f};
pub.publish(pt);
```

### Subscribe (consumer process)

```cpp
#include "lwipc/lwipc.hpp"
using namespace lwipc;

Subscriber<PointXYZI, 128> sub("/lidar");
sub.start_async([](const IPCHeader& h, const PointXYZI& p) {
    printf("seq=%lu x=%.2f\n", h.sequence, p.x);
});
// ... sub.stop() when done
```

### Cross-host via TCP

```cpp
// Server (receiver)
SocketServer srv(7788);
srv.start([](const IPCMessage& msg) { /* handle */ });

// Client (sender)
SocketClient cli("192.168.1.10", 7788);
cli.connect();
cli.send(IPCMessage::make(MsgType::IMU, 5, ts_ns, seq, imu_sample));
```

---

## Modules

### `IPCHeader` (64 bytes, cache-line aligned)
| Field | Type | Description |
|---|---|---|
| `magic` | `uint32_t` | Frame marker `"LWIP"` |
| `version` | `uint8_t` | Protocol version |
| `msg_type` | `MsgType` | `IMAGE/POINTCLOUD/IMU/TENSOR/CONTROL_CMD` |
| `sensor_id` | `uint16_t` | Source sensor identifier |
| `timestamp_ns` | `uint64_t` | Unix nanoseconds |
| `sequence` | `uint64_t` | Monotonic per-producer counter |
| `data_length` | `uint32_t` | Payload byte length |
| `checksum` | `uint32_t` | CRC-32 over payload |

### `SyncPolicy` variants
- `RelaxedPolicy` — relaxed ordering (single-thread / benchmark use)
- `AcqRelPolicy` — acquire/release (default, correct for SPMC)
- `SeqCstPolicy` — sequentially consistent (strongest)

### `ShmRingBuffer<T, Capacity, Policy>`
- `Capacity` must be power of two
- `push(item)` — producer, never blocks
- `pop(cursor, out)` — consumer, non-blocking, returns bool
- `make_cursor()` — start from current write head (latest-only semantics)
- Auto fast-forwards stale consumers to avoid starvation

---

## Tests & Benchmarks

```
tests/
  test_ipc_message.cpp        29 assertions — header, CRC, serialisation
  test_shm_ring_buffer.cpp    38 assertions — SPMC, wrap-around, multi-consumer
  test_pub_sub.cpp            20 assertions — typed pub/sub, async callback
  test_socket_transport.cpp   28 assertions — TCP roundtrip, checksum

examples/
  benchmark.cpp               100 000 messages, latency histogram
  sensor_publisher.cpp        LiDAR publisher demo (100 Hz)
  ai_subscriber.cpp           Inference + control publisher demo
  socket_bridge.cpp           TCP receive-and-print server
```

**Benchmark results (same process, Release build, Linux x86-64):**
```
avg latency : ~150 ns
p99 latency : ~160 ns
throughput  : > 90 GB/s
```
