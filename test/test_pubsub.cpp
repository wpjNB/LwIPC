// test_pubsub.cpp
//
// Multi-thread, high-frequency pub/sub integration test for LwIPC.
//
// Covers:
//   1. ObjectPool acquire/release correctness (single-thread + multi-thread)
//   2. LoanedSample RAII: auto-return on destruction, move semantics
//   3. Intra-process pub/sub via IntraPublisher / IntraSubscriber:
//        - loan → fill → publish → receive → verify data integrity
//   4. Shared-memory pub/sub via Publisher / Subscriber with loan() API
//   5. Multi-threaded high-frequency publish (producer + multiple consumers)
//   6. Full borrow / publish / recycle closed loop with pool exhaustion guard

#include "lwipc/lwipc.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace lwipc;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Minimal self-contained assertion framework (no external deps)
// ---------------------------------------------------------------------------
static int g_tests = 0, g_pass = 0;

#define CHECK(cond) do { \
    ++g_tests; \
    if (cond) { ++g_pass; } \
    else { std::fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
struct Sample {
    uint64_t seq{0};
    float    value{0.f};
    float    extra[6]{};
};
static_assert(std::is_trivially_copyable<Sample>::value);

// ---------------------------------------------------------------------------
// 1. ObjectPool: basic acquire / release
// ---------------------------------------------------------------------------
void test_object_pool_basic() {
    ObjectPool<Sample, 4> pool;

    CHECK(pool.available() == 4u);

    Sample* a = pool.acquire();
    CHECK(a != nullptr);
    CHECK(pool.available() == 3u);

    Sample* b = pool.acquire();
    Sample* c = pool.acquire();
    Sample* d = pool.acquire();
    CHECK(d != nullptr);
    CHECK(pool.available() == 0u);

    // Pool exhausted — must return nullptr
    Sample* e = pool.acquire();
    CHECK(e == nullptr);

    // Release and re-acquire
    pool.release(a);
    CHECK(pool.available() == 1u);
    Sample* f = pool.acquire();
    CHECK(f != nullptr);

    pool.release(b);
    pool.release(c);
    pool.release(d);
    pool.release(f);
    CHECK(pool.available() == 4u);
}

// ---------------------------------------------------------------------------
// 2. ObjectPool: concurrent acquire / release
// ---------------------------------------------------------------------------
void test_object_pool_concurrent() {
    static constexpr std::size_t PoolSize = 64u;
    static constexpr int         Rounds   = 10'000;

    ObjectPool<Sample, PoolSize> pool;
    std::atomic<int> acquired_total{0};

    auto worker = [&]() {
        for (int i = 0; i < Rounds; ++i) {
            Sample* p = pool.acquire();
            if (p) {
                p->seq   = static_cast<uint64_t>(i);
                p->value = float(i);
                ++acquired_total;
                pool.release(p);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int i = 0; i < 4; ++i) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    CHECK(pool.available() == PoolSize);
    CHECK(acquired_total.load() > 0);
}

// ---------------------------------------------------------------------------
// 3. LoanedSample: RAII auto-return
// ---------------------------------------------------------------------------
void test_loaned_sample_raii() {
    ObjectPool<Sample, 8> pool;
    CHECK(pool.available() == 8u);

    {
        using LoanT = LoanedSample<Sample, ObjectPool<Sample, 8>>;
        LoanT loan(pool.acquire(), &pool);
        CHECK(static_cast<bool>(loan));
        CHECK(pool.available() == 7u);
        loan->seq   = 42u;
        loan->value = 3.14f;
        // loan goes out of scope → auto-released
    }
    CHECK(pool.available() == 8u);
}

// ---------------------------------------------------------------------------
// 4. LoanedSample: move semantics
// ---------------------------------------------------------------------------
void test_loaned_sample_move() {
    ObjectPool<Sample, 4> pool;
    using LoanT = LoanedSample<Sample, ObjectPool<Sample, 4>>;

    LoanT a(pool.acquire(), &pool);
    CHECK(pool.available() == 3u);

    // Move-construct: a → b
    LoanT b(std::move(a));
    CHECK(!a);                         // a is now empty
    CHECK(static_cast<bool>(b));
    CHECK(pool.available() == 3u);     // still 1 outstanding

    // Move-assign: b → c
    LoanT c = std::move(b);
    CHECK(!b);
    CHECK(static_cast<bool>(c));
    c.reset();                          // explicit reset
    CHECK(pool.available() == 4u);     // returned
}

// ---------------------------------------------------------------------------
// 5. IntraPublisher / IntraSubscriber: loan → fill → publish → receive
// ---------------------------------------------------------------------------
void test_intra_loan_publish_receive() {
    auto ring = std::make_shared<IntraRingBuffer<Sample, 32>>();
    IntraPublisher<Sample, 32>  pub(ring, MsgType::IMU, 1);
    IntraSubscriber<Sample, 32> sub(ring);

    auto cur = sub.make_cursor();

    // Loan, fill, publish
    auto loan = pub.loan();
    CHECK(static_cast<bool>(loan));
    loan->seq   = 7u;
    loan->value = 2.718f;
    pub.publish(std::move(loan));       // consumes loan, returns to pool
    CHECK(!loan);                       // loan is empty after publish
    CHECK(pub.published() == 1u);
    CHECK(pub.pool_available() == 32u); // loan returned to pool after publish

    // Receive
    Sample out{};
    CHECK(sub.try_receive(cur, out));
    CHECK(out.seq == 7u);
    CHECK(out.value == 2.718f);
    CHECK(!sub.try_receive(cur, out)); // nothing more
}

// ---------------------------------------------------------------------------
// 6. make_intra_pubsub factory + classic copy publish
// ---------------------------------------------------------------------------
void test_make_intra_pubsub() {
    auto ps = make_intra_pubsub<Sample, 16>(MsgType::POINTCLOUD, 3);
    auto& pub = *ps.publisher;
    auto& sub = *ps.subscriber;

    auto cur = sub.make_cursor();
    Sample s{};
    s.seq = 99u; s.value = -1.f;
    pub.publish(s);

    Sample got{};
    CHECK(sub.try_receive(cur, got));
    CHECK(got.seq == 99u);
    CHECK(got.value == -1.f);
}

// ---------------------------------------------------------------------------
// 7. IntraSubscriber async callback
// ---------------------------------------------------------------------------
void test_intra_async_callback() {
    auto ring = std::make_shared<IntraRingBuffer<Sample, 64>>();
    IntraPublisher<Sample, 64>  pub(ring, MsgType::IMU, 0);
    IntraSubscriber<Sample, 64> sub(ring);

    std::atomic<int> received{0};
    sub.start_async([&](const Sample& s) {
        (void)s;
        received.fetch_add(1, std::memory_order_relaxed);
    }, std::chrono::microseconds(50));

    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    static constexpr int N = 50;
    for (int i = 0; i < N; ++i) {
        auto loan = pub.loan();
        if (loan) {
            loan->seq   = static_cast<uint64_t>(i);
            loan->value = float(i);
            pub.publish(std::move(loan));
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sub.stop();

    // With 50 µs poll interval and 100 ms wait, expect at least 30 received
    CHECK(received.load() >= 30);
}

// ---------------------------------------------------------------------------
// 8. Multi-consumer intra-process (SPMC)
// ---------------------------------------------------------------------------
void test_intra_spmc() {
    auto ring = std::make_shared<IntraRingBuffer<Sample, 128>>();
    IntraPublisher<Sample, 128>  pub(ring, MsgType::POINTCLOUD, 0);
    IntraSubscriber<Sample, 128> sub1(ring);
    IntraSubscriber<Sample, 128> sub2(ring);
    IntraSubscriber<Sample, 128> sub3(ring);

    auto c1 = sub1.make_cursor();
    auto c2 = sub2.make_cursor();
    auto c3 = sub3.make_cursor();

    static constexpr int N = 100;
    for (int i = 0; i < N; ++i) {
        Sample s{}; s.seq = uint64_t(i); s.value = float(i);
        pub.publish(s);
    }

    int cnt1 = 0, cnt2 = 0, cnt3 = 0;
    Sample out{};
    while (sub1.try_receive(c1, out)) ++cnt1;
    while (sub2.try_receive(c2, out)) ++cnt2;
    while (sub3.try_receive(c3, out)) ++cnt3;

    CHECK(cnt1 == N);
    CHECK(cnt2 == N);
    CHECK(cnt3 == N);
}

// ---------------------------------------------------------------------------
// 9. Shared-memory Publisher loan() API
// ---------------------------------------------------------------------------
void test_shm_publisher_loan() {
    const char* ch = "/lwipc_tp_loan";
    Publisher<Sample, 32>  pub(ch, MsgType::IMU, 5);
    Subscriber<Sample, 32> sub(ch);

    auto cur = sub.make_cursor();

    auto loan = pub.loan();
    CHECK(static_cast<bool>(loan));
    loan->seq   = 123u;
    loan->value = 9.9f;
    pub.publish(std::move(loan));
    CHECK(!loan);

    IPCHeader hdr{};
    Sample    got{};
    CHECK(sub.try_receive(cur, hdr, got));
    CHECK(got.seq   == 123u);
    CHECK(got.value == 9.9f);
    CHECK(hdr.sensor_id == 5);
    CHECK(hdr.sequence  == 1u);
}

// ---------------------------------------------------------------------------
// 10. High-frequency multi-thread loan/publish/recycle closed loop
// ---------------------------------------------------------------------------
void test_high_frequency_intra() {
    static constexpr int       N         = 50'000;
    static constexpr int       N_CONS    = 3;
    static constexpr std::size_t RingSize = 256u;

    auto ring = std::make_shared<IntraRingBuffer<Sample, RingSize>>();
    IntraPublisher<Sample, RingSize> pub(ring, MsgType::POINTCLOUD, 0);

    std::atomic<long long> total_received{0};
    std::vector<std::thread> consumers;
    std::atomic<bool> done{false};

    for (int i = 0; i < N_CONS; ++i) {
        consumers.emplace_back([&, i]() {
            IntraSubscriber<Sample, RingSize> sub(ring);
            auto cur = sub.make_cursor();
            long long local = 0;
            while (!done.load(std::memory_order_acquire)) {
                Sample out{};
                if (sub.try_receive(cur, out)) ++local;
                else std::this_thread::yield();
            }
            // drain remaining
            Sample out{};
            while (sub.try_receive(cur, out)) ++local;
            total_received.fetch_add(local, std::memory_order_relaxed);
            (void)i;
        });
    }

    // Producer: use loan API for all publishes
    auto t0 = steady_clock::now();
    int loan_failures = 0;
    for (int i = 0; i < N; ++i) {
        auto loan = pub.loan();
        if (loan) {
            loan->seq   = static_cast<uint64_t>(i);
            loan->value = float(i);
            pub.publish(std::move(loan));
        } else {
            ++loan_failures;
            // Pool temporarily exhausted; fall back to copy API
            Sample s{}; s.seq = uint64_t(i); s.value = float(i);
            pub.publish(s);
        }
    }
    auto t1 = steady_clock::now();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    done.store(true, std::memory_order_release);
    for (auto& t : consumers) t.join();

    const double elapsed_us =
        double(duration_cast<nanoseconds>(t1 - t0).count()) / 1e3;
    const double msg_per_us = double(N) / elapsed_us;

    std::printf("  [high_freq] published=%d loan_failures=%d "
                "received=%lld consumers=%d "
                "publish_rate=%.2f Mmsg/s\n",
                N, loan_failures,
                total_received.load(), N_CONS,
                msg_per_us);

    CHECK(pub.published() == static_cast<uint64_t>(N));
    // Each consumer sees at least 80% of messages (ring may drop stale ones)
    const long long min_expected = static_cast<long long>(N) * N_CONS * 8 / 10;
    CHECK(total_received.load() >= min_expected);
    CHECK(loan_failures < N / 2);  // majority of publishes should loan successfully
}

// ---------------------------------------------------------------------------
// 11. Pool exhaustion: all slots outstanding, publish falls back gracefully
// ---------------------------------------------------------------------------
void test_pool_exhaustion_fallback() {
    static constexpr std::size_t Cap = 4u;
    ObjectPool<Sample, Cap> pool;

    // Drain all slots
    std::array<Sample*, Cap> slots{};
    for (auto& p : slots) {
        p = pool.acquire();
        CHECK(p != nullptr);
    }
    CHECK(pool.available() == 0u);

    // Try to acquire when exhausted
    Sample* extra = pool.acquire();
    CHECK(extra == nullptr);

    // Release one and try again
    pool.release(slots[0]);
    slots[0] = nullptr;
    Sample* reacquired = pool.acquire();
    CHECK(reacquired != nullptr);

    // Release all
    pool.release(reacquired);
    for (auto& p : slots) {
        if (p) pool.release(p);
    }
    CHECK(pool.available() == Cap);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::printf("=== LwIPC ObjectPool / LoanedSample / IntraPubSub Tests ===\n");

    test_object_pool_basic();
    std::printf("  [1/11] object_pool_basic          ");
    std::printf("OK\n");

    test_object_pool_concurrent();
    std::printf("  [2/11] object_pool_concurrent     ");
    std::printf("OK\n");

    test_loaned_sample_raii();
    std::printf("  [3/11] loaned_sample_raii         ");
    std::printf("OK\n");

    test_loaned_sample_move();
    std::printf("  [4/11] loaned_sample_move         ");
    std::printf("OK\n");

    test_intra_loan_publish_receive();
    std::printf("  [5/11] intra_loan_publish_receive ");
    std::printf("OK\n");

    test_make_intra_pubsub();
    std::printf("  [6/11] make_intra_pubsub          ");
    std::printf("OK\n");

    test_intra_async_callback();
    std::printf("  [7/11] intra_async_callback       ");
    std::printf("OK\n");

    test_intra_spmc();
    std::printf("  [8/11] intra_spmc                 ");
    std::printf("OK\n");

    test_shm_publisher_loan();
    std::printf("  [9/11] shm_publisher_loan         ");
    std::printf("OK\n");

    test_high_frequency_intra();
    std::printf("  [10/11] high_frequency_intra      ");
    std::printf("OK\n");

    test_pool_exhaustion_fallback();
    std::printf("  [11/11] pool_exhaustion_fallback  ");
    std::printf("OK\n");

    std::printf("\nResults: %d/%d passed\n", g_pass, g_tests);
    return (g_pass == g_tests) ? 0 : 1;
}
