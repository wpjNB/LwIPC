// test_shm_ring_buffer.cpp
// Unit / integration tests for ShmRingBuffer (lock-free, SPMC, POSIX shm).

#include "lwipc/shm_ring_buffer.hpp"
#include "lwipc/sync_policy.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace lwipc;

static int g_tests = 0, g_pass = 0;

#define CHECK(cond)  do { \
    ++g_tests; \
    if (cond) { ++g_pass; } \
    else { std::fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while(0)

// ---------------------------------------------------------------------------
struct Msg { uint64_t seq; float value; };

// ---------------------------------------------------------------------------
void test_single_push_pop() {
    const char* name = "/lwipc_test_spsc";
    ShmRingBuffer<Msg, 8> producer(name, true);
    ShmRingBuffer<Msg, 8> consumer(name, false);

    auto cur = consumer.make_cursor();
    Msg m{};
    CHECK(!consumer.pop(cur, m));  // nothing yet

    producer.push({1, 3.14f});
    CHECK(consumer.pop(cur, m));
    CHECK(m.seq == 1);
    CHECK(m.value == 3.14f);
    CHECK(!consumer.pop(cur, m));  // consumed, nothing more
}

void test_multiple_messages_ordered() {
    const char* name = "/lwipc_test_ordered";
    ShmRingBuffer<Msg, 16> prod(name, true);
    ShmRingBuffer<Msg, 16> cons(name, false);

    auto cur = cons.make_cursor();
    for (uint64_t i = 1; i <= 10; ++i) prod.push({i, float(i)});

    for (uint64_t i = 1; i <= 10; ++i) {
        Msg m{};
        CHECK(cons.pop(cur, m));
        CHECK(m.seq == i);
    }
    Msg m{};
    CHECK(!cons.pop(cur, m));
}

void test_ring_wrap_around() {
    const char* name = "/lwipc_test_wrap";
    ShmRingBuffer<Msg, 4> prod(name, true);
    ShmRingBuffer<Msg, 4> cons(name, false);

    auto cur = cons.make_cursor();

    // Overfill — capacity is 4, write 8 messages so the first 4 are overwritten.
    for (uint64_t i = 1; i <= 8; ++i) prod.push({i, float(i)});

    // After fast-forward, cursor.pos should be adjusted to write_pos - Capacity
    // so that the consumer only sees the last 4 messages (or fewer due to
    // sequence-check timing).  Verify at least: cursor is within valid range.
    Msg m{};
    // Drain whatever is available
    int got_count = 0;
    while (cons.pop(cur, m)) ++got_count;

    // After draining, cursor must equal write_pos (nothing left)
    CHECK(cur.pos == prod.write_pos());
    // Must have received at most Capacity messages (the ring is 4 slots)
    CHECK(got_count <= 4);
    // Must have received at least 1 (the latest slot is always readable)
    CHECK(got_count >= 1);
}

void test_multiple_consumers() {
    const char* name = "/lwipc_test_mc";
    ShmRingBuffer<Msg, 32> prod(name, true);
    ShmRingBuffer<Msg, 32> cons1(name, false);
    ShmRingBuffer<Msg, 32> cons2(name, false);

    auto cur1 = cons1.make_cursor();
    auto cur2 = cons2.make_cursor();

    for (uint64_t i = 1; i <= 5; ++i) prod.push({i, float(i)});

    uint64_t sum1 = 0, sum2 = 0;
    Msg m{};
    while (cons1.pop(cur1, m)) sum1 += m.seq;
    while (cons2.pop(cur2, m)) sum2 += m.seq;

    CHECK(sum1 == 1+2+3+4+5);
    CHECK(sum2 == 1+2+3+4+5);
}

void test_spmc_concurrent() {
    const char* name = "/lwipc_test_spmc";
    static constexpr int N = 1000;
    static constexpr int C = 4;  // consumers

    ShmRingBuffer<Msg, 256> prod(name, true);
    std::vector<std::atomic<uint64_t>> counts(C);
    for (auto& a : counts) a.store(0);

    // Spawn consumers first
    std::vector<std::thread> cthreads;
    std::atomic<bool> done{false};
    for (int i = 0; i < C; ++i) {
        cthreads.emplace_back([&, i]() {
            ShmRingBuffer<Msg, 256> cons(name, false);
            auto cur = cons.make_cursor();
            while (!done.load(std::memory_order_acquire)) {
                Msg m{};
                if (cons.pop(cur, m)) {
                    counts[i].fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
            // drain
            Msg m{};
            while (cons.pop(cur, m)) counts[i].fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Producer
    for (int i = 1; i <= N; ++i) {
        prod.push({uint64_t(i), float(i)});
        if (i % 100 == 0) std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
    // Give consumers time to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    done.store(true, std::memory_order_release);
    for (auto& t : cthreads) t.join();

    // Each consumer should have received at least 1 message (SPMC: each gets its own copy)
    for (int i = 0; i < C; ++i) {
        CHECK(counts[i].load() > 0);
    }
}

void test_relaxed_policy() {
    const char* name = "/lwipc_test_relax";
    ShmRingBuffer<Msg, 8, RelaxedPolicy> prod(name, true);
    ShmRingBuffer<Msg, 8, RelaxedPolicy> cons(name, false);
    auto cur = cons.make_cursor();
    prod.push({42, 1.f});
    Msg m{};
    CHECK(cons.pop(cur, m));
    CHECK(m.seq == 42);
}

void test_seqcst_policy() {
    const char* name = "/lwipc_test_seqcst";
    ShmRingBuffer<Msg, 8, SeqCstPolicy> prod(name, true);
    ShmRingBuffer<Msg, 8, SeqCstPolicy> cons(name, false);
    auto cur = cons.make_cursor();
    prod.push({99, 2.f});
    Msg m{};
    CHECK(cons.pop(cur, m));
    CHECK(m.seq == 99);
}

// ---------------------------------------------------------------------------
int main() {
    test_single_push_pop();
    test_multiple_messages_ordered();
    test_ring_wrap_around();
    test_multiple_consumers();
    test_spmc_concurrent();
    test_relaxed_policy();
    test_seqcst_policy();

    std::printf("ShmRingBuffer tests: %d/%d passed\n", g_pass, g_tests);
    return (g_pass == g_tests) ? 0 : 1;
}
