// test_ipc_message.cpp
// Unit tests for IPCHeader, IPCMessage, CRC-32, typed payload helpers.
// Uses a simple self-contained assertion framework (no external deps).

#include "lwipc/ipc_message.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace lwipc;

// ---------------------------------------------------------------------------
static int g_tests = 0, g_pass = 0;

#define CHECK(cond)  do { \
    ++g_tests; \
    if (cond) { ++g_pass; } \
    else { std::fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while(0)

// ---------------------------------------------------------------------------
void test_header_size_and_magic() {
    CHECK(sizeof(IPCHeader) == 64);
    IPCHeader h{};
    CHECK(h.valid());
    CHECK(h.magic == IPCHeader::kMagic);
    CHECK(h.version == IPCHeader::kVersion);
}

void test_make_and_verify() {
    PointXYZI pt{1.f, 2.f, 3.f, 0.9f};
    auto msg = IPCMessage::make(MsgType::POINTCLOUD, /*sensor_id=*/3,
                                /*ts=*/1'000'000'000ULL, /*seq=*/1, pt);
    CHECK(msg.valid());
    CHECK(msg.header().msg_type   == MsgType::POINTCLOUD);
    CHECK(msg.header().sensor_id  == 3);
    CHECK(msg.header().sequence   == 1);
    CHECK(msg.header().data_length == sizeof(PointXYZI));
    CHECK(msg.verify_checksum());
}

void test_as_typed_payload() {
    ImuSample imu{};
    imu.accel[0] = 0.1f; imu.gyro[2] = 3.14f;
    auto msg = IPCMessage::make(MsgType::IMU, 5, 0, 0, imu);
    const ImuSample* p = msg.as<ImuSample>();
    CHECK(p != nullptr);
    CHECK(p->accel[0] == 0.1f);
    CHECK(p->gyro[2]  == 3.14f);
}

void test_serialise_roundtrip() {
    ControlCmd cmd{0.5f, 0.8f, 0.0f, 3};
    auto original = IPCMessage::make(MsgType::CONTROL_CMD, 0, 99, 7, cmd);

    auto buf      = original.to_buffer();
    CHECK(buf.size() == sizeof(IPCHeader) + sizeof(ControlCmd));

    auto restored = IPCMessage::from_buffer(buf.data(), buf.size());
    CHECK(restored.valid());
    CHECK(restored.verify_checksum());
    CHECK(restored.header().sequence   == 7);
    CHECK(restored.header().sensor_id  == 0);
    CHECK(restored.header().msg_type   == MsgType::CONTROL_CMD);

    const ControlCmd* c = restored.as<ControlCmd>();
    CHECK(c != nullptr);
    CHECK(c->steering  == 0.5f);
    CHECK(c->throttle  == 0.8f);
    CHECK(c->gear      == 3);
}

void test_bad_magic_rejected() {
    PointXYZI pt{};
    auto msg = IPCMessage::make(MsgType::IMAGE, 0, 0, 0, pt);
    auto buf = msg.to_buffer();
    // Corrupt magic
    buf[0] = 0xDE; buf[1] = 0xAD;
    auto bad = IPCMessage::from_buffer(buf.data(), buf.size());
    CHECK(!bad.valid());
}

void test_checksum_detects_corruption() {
    PointXYZI pt{1.f, 2.f, 3.f, 4.f};
    auto msg = IPCMessage::make(MsgType::POINTCLOUD, 0, 0, 0, pt);
    auto buf = msg.to_buffer();
    // Flip a byte in the payload region
    buf[sizeof(IPCHeader) + 2] ^= 0xFF;
    auto corrupted = IPCMessage::from_buffer(buf.data(), buf.size());
    CHECK(corrupted.valid());          // header is still intact
    CHECK(!corrupted.verify_checksum()); // but checksum fails
}

void test_empty_payload() {
    IPCHeader h{};
    h.msg_type = MsgType::UNKNOWN;
    IPCMessage msg(h, nullptr, 0);
    CHECK(msg.valid());
    CHECK(msg.data().empty());
    CHECK(msg.verify_checksum());
}

// ---------------------------------------------------------------------------
int main() {
    test_header_size_and_magic();
    test_make_and_verify();
    test_as_typed_payload();
    test_serialise_roundtrip();
    test_bad_magic_rejected();
    test_checksum_detects_corruption();
    test_empty_payload();

    std::printf("IPCMessage tests: %d/%d passed\n", g_pass, g_tests);
    return (g_pass == g_tests) ? 0 : 1;
}
