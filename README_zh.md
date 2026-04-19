# LwIPC — 面向自动驾驶的轻量级进程间通信框架

基于 **POSIX 共享内存** + **无锁环形缓冲区** 以及 **对象池 / 零拷贝借用 API** 的 C++17 IPC 框架，专为自动驾驶模块（传感器采集、AI 推理、运动控制）之间的高性能进程间（及进程内）通信设计。

---

## 特性

| 特性 | 说明 |
|---|---|
| **传输方式** | POSIX `shm_open` / `mmap`（本机）+ TCP Socket（跨主机）+ 堆内环形缓冲区（进程内） |
| **数据结构** | 无锁 SPMC 环形缓冲区 |
| **零拷贝** | `ObjectPool` + `LoanedSample` 借用/发布/回收模式 |
| **同步原语** | `std::atomic`，内存序可配置 |
| **API 风格** | C++17，模板，RAII，无异常，无全局变量 |
| **消息类型** | `PointCloud`、`Image`、`IMU`、`Tensor`、`ControlCmd` |
| **延迟** | ~150 ns（同进程，实测） |
| **吞吐量** | > 90 GB/s（同进程，实测） |

---

## 架构

```
┌──────────────────────────────────────────────────────────────┐
│                      LwIPC 模块                              │
│                                                              │
│  IPCMessage      结构化头部 + 载荷 + CRC-32                  │
│  SyncPolicy      内存序封装                                  │
│  ObjectPool      固定大小无锁预分配对象池                    │
│  LoanedSample    RAII 借用/自动归还包装器                    │
│  ShmRingBuffer   基于 POSIX 共享内存的无锁 SPMC 环形缓冲区   │
│  IntraRingBuffer 基于堆内存的无锁 SPMC 环形缓冲区            │
│  Publisher       RAII 写 API（共享内存通道，支持 loan() API）│
│  IntraPublisher  RAII 写 API（堆内通道，支持 loan() API）    │
│  Subscriber      RAII 读 API（挂载共享内存通道）             │
│  IntraSubscriber RAII 读 API（挂载堆内通道）                 │
│  SocketTransport TCP 封帧（SocketServer / SocketClient）     │
└──────────────────────────────────────────────────────────────┘
```

### 零拷贝借用模型

```
Publisher/IntraPublisher
  │
  ├─ loan() ──────────────────────┐
  │                              ↓
  │                         ObjectPool<T, N>
  │                          T* ptr = acquire()
  │                              │
  │   调用方就地填充 *ptr（无内存分配，无拷贝）
  │                              │
  └─ publish(LoanedSample&&) ────┘
       │
       ├─ 仅一次拷贝：*ptr → 环形槽位
       └─ pool.release(ptr)  ← RAII 或手动释放

订阅方读取环形槽位（const T&）——从生产者到消费者共一次拷贝。
```

### ShmRingBuffer — 无锁 SPMC 设计

```
共享内存布局：
  [ ControlBlock (64 B) | Slot[0] … Slot[N-1] ]

ControlBlock: atomic<uint64_t> write_pos

每个 Slot：
  atomic<uint64_t> sequence   （偶数 = 空/写入中，奇数 = 就绪）
  T                data        （载荷，零拷贝）
  uint8_t          _pad[…]    （缓存行对齐填充）

生产者：fetch_add(write_pos) → sequence(偶数) → 拷贝 → sequence(奇数)
消费者：检查 sequence == 预期奇数 → 拷贝 → ++cursor
```

---

## 目录结构

```
include/lwipc/
  object_pool.hpp      — 无锁固定大小对象池模板
  loaned_sample.hpp    — RAII 借用/自动归还包装器
  publisher.hpp        — Publisher（共享内存）+ IntraPublisher（堆内）+ loan() API
  subscriber.hpp       — Subscriber（共享内存）+ IntraSubscriber（堆内）+ 工厂函数
  shm_ring_buffer.hpp  — 基于 POSIX 共享内存的无锁 SPMC 环形缓冲区
  ipc_message.hpp      — IPCHeader、类型化载荷、CRC-32
  sync_policy.hpp      — 内存序策略（Relaxed / AcqRel / SeqCst）
  socket_transport.hpp — TCP 封帧（SocketServer / SocketClient）
  lwipc.hpp            — 总头文件

src/
  object_pool.cpp      — 编译单元（仅头文件；TODO 存根）
  loaned_sample.cpp    — 编译单元（仅头文件；TODO 存根）
  publisher.cpp        — 编译单元（仅头文件；TODO 存根）
  subscriber.cpp       — 编译单元（仅头文件；TODO 存根）

test/
  test_pubsub.cpp      — 11 个进程内发布/订阅 + 对象池/借用测试场景

tests/
  test_ipc_message.cpp        — 头部 / CRC / 序列化（29 项检查）
  test_shm_ring_buffer.cpp    — SPMC、回绕、多消费者（39 项检查）
  test_pub_sub.cpp            — 类型化共享内存发布/订阅、异步回调（20 项检查）
  test_socket_transport.cpp   — TCP 往返、校验和（28 项检查）

examples/
  benchmark.cpp        — 100 000 条消息，延迟直方图
  sensor_publisher.cpp — 激光雷达发布者示例（100 Hz）
  ai_subscriber.cpp    — 推理 + 控制发布者示例
  socket_bridge.cpp    — TCP 接收打印服务器
```

---

## 快速开始

### 编译

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure     # 运行全部测试
```

### 进程内零拷贝（借用 API）——最快路径

```cpp
#include "lwipc/lwipc.hpp"
using namespace lwipc;

// 创建共享环形缓冲区及发布者/订阅者对
auto ring = std::make_shared<IntraRingBuffer<PointXYZI, 64>>();
IntraPublisher<PointXYZI, 64>  pub(ring, MsgType::POINTCLOUD, 1);
IntraSubscriber<PointXYZI, 64> sub(ring);
auto cur = sub.make_cursor();

// 借用一个槽位，就地填充，发布（无内存分配，仅一次拷贝到环形缓冲区）
auto loan = pub.loan();
if (loan) {
    loan->x = 1.f;  loan->y = 2.f;  loan->z = 3.f;  loan->intensity = 0.8f;
    pub.publish(std::move(loan));   // loan 自动归还对象池
}

// 接收
PointXYZI pt;
if (sub.try_receive(cur, pt)) { /* 使用 pt */ }
```

### 便捷工厂函数

```cpp
auto ps = make_intra_pubsub<PointXYZI, 64>(MsgType::POINTCLOUD, 1);
auto& pub = *ps.publisher;
auto& sub = *ps.subscriber;
```

### 异步回调（进程内）

```cpp
sub.start_async([](const PointXYZI& p) {
    printf("x=%.2f\n", p.x);
}, std::chrono::microseconds(100));
// … 稍后 …
sub.stop();
```

### 共享内存跨进程通信（经典 API）

```cpp
// 生产者进程
Publisher<PointXYZI, 128> pub("/lidar", MsgType::POINTCLOUD, 1);
PointXYZI pt{1.f, 2.f, 3.f, 0.8f};
pub.publish(pt);

// 生产者 —— 使用借用 API 零拷贝写入
auto loan = pub.loan();
if (loan) { loan->x = 1.f; pub.publish(std::move(loan)); }
```

```cpp
// 消费者进程
Subscriber<PointXYZI, 128> sub("/lidar");
sub.start_async([](const IPCHeader& h, const PointXYZI& p) {
    printf("seq=%lu x=%.2f\n", h.sequence, p.x);
});
// ... 完成后调用 sub.stop()
```

### 通过 TCP 跨主机通信

```cpp
// 服务端（接收方）
SocketServer srv(7788);
srv.start([](const IPCMessage& msg) { /* 处理消息 */ });

// 客户端（发送方）
SocketClient cli("192.168.1.10", 7788);
cli.connect();
cli.send(IPCMessage::make(MsgType::IMU, 5, ts_ns, seq, imu_sample));
```

---

## 模块说明

### `ObjectPool<T, Capacity>`
| 方法 | 说明 |
|---|---|
| `acquire()` | 借用一个槽位；耗尽时返回 `nullptr` |
| `release(T*)` | 将槽位归还对象池 |
| `available()` | 空闲槽位的近似数量 |

- 基于对齐静态存储（构造后不再进行堆分配）
- 无锁 LIFO 空闲链表，配合 ABA 安全的带标签指针
- `Capacity` 必须为 2 的幂，且 ≤ 65535

### `LoanedSample<T, Pool>`
- 仅可移动的 RAII 包装器；析构时自动调用 `pool.release(ptr)`
- 支持 `operator->()`、`operator*()`、`explicit operator bool()`
- `reset()` — 提前手动归还；`release_raw()` — 转移所有权

### `IPCHeader`（64 字节，缓存行对齐）
| 字段 | 类型 | 说明 |
|---|---|---|
| `magic` | `uint32_t` | 帧标记 `"LWIP"` |
| `version` | `uint8_t` | 协议版本 |
| `msg_type` | `MsgType` | `IMAGE/POINTCLOUD/IMU/TENSOR/CONTROL_CMD` |
| `sensor_id` | `uint16_t` | 来源传感器标识符 |
| `timestamp_ns` | `uint64_t` | Unix 纳秒时间戳 |
| `sequence` | `uint64_t` | 生产者单调递增计数器 |
| `data_length` | `uint32_t` | 载荷字节长度 |
| `checksum` | `uint32_t` | 载荷的 CRC-32 校验值 |

### `SyncPolicy` 变体
- `RelaxedPolicy` — 宽松内存序（单线程 / 基准测试使用）
- `AcqRelPolicy` — 获取/释放语义（默认，SPMC 下正确）
- `SeqCstPolicy` — 顺序一致性（最强）

### `ShmRingBuffer<T, Capacity, Policy>`
- `Capacity` 必须为 2 的幂
- `push(item)` — 生产者写入，永不阻塞
- `pop(cursor, out)` — 消费者非阻塞读取，返回 bool
- `make_cursor()` — 从当前写入头开始（仅获取最新数据语义）
- 自动将落后的消费者快进，防止饥饿

### `Publisher<T, Capacity>` / `Subscriber<T, Capacity>`
- 基于 `ShmRingBuffer`（POSIX 共享内存，支持跨进程）
- `Publisher::loan()` — 从内部 `ObjectPool` 借用，就地填充
- `Publisher::publish(LoanedSample&&)` — 零拷贝写入并自动释放

### `IntraPublisher<T, Capacity>` / `IntraSubscriber<T, Capacity>`
- 基于 `IntraRingBuffer`（堆内存，仅限进程内，开销最小）
- 与 `Publisher` 相同的借用/发布 API
- `make_intra_pubsub<T, N>(msg_type, sensor_id)` — 便捷工厂函数

---

## 测试与基准测试

```
test/
  test_pubsub.cpp             55 项检查 — 对象池、借用 RAII、进程内发布/订阅、
                                           高频多线程、对象池耗尽

tests/
  test_ipc_message.cpp        29 项检查 — 头部、CRC、序列化
  test_shm_ring_buffer.cpp    39 项检查 — SPMC、回绕、多消费者
  test_pub_sub.cpp            20 项检查 — 类型化发布/订阅、异步回调
  test_socket_transport.cpp   28 项检查 — TCP 往返、校验和

examples/
  benchmark.cpp               100 000 条消息，延迟直方图
  sensor_publisher.cpp        激光雷达发布者示例（100 Hz）
  ai_subscriber.cpp           推理 + 控制发布者示例
  socket_bridge.cpp           TCP 接收打印服务器
```

**基准测试结果（同进程，Release 构建，Linux x86-64）：**
```
平均延迟：~150 ns
P99 延迟：~160 ns
吞吐量：  > 90 GB/s
```
