#include "protocol.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <thread>

namespace {

// Print `msg` to stderr and return 1.
int Fail(const char* msg) {
	std::fprintf(stderr, "%s\n", msg);
	return 1;
}

struct Region {
	unsigned char* bytes;

	// Allocate and init a default-sized region. `bytes` is null if init fails.
	Region() : bytes(static_cast<unsigned char*>(::operator new(kDefaultBytes, std::align_val_t{128}))) {
		if (!InitRegion(bytes, kDefaultBytes)) {
			std::fprintf(stderr, "init failed\n");
			bytes = nullptr;
		}
	}

	// Free the allocation.
	~Region() { ::operator delete(bytes, std::align_val_t{128}); }

	// The region header.
	Header* header() { return reinterpret_cast<Header*>(bytes); }
	// The command ring.
	Ring cmd() { return Ring(bytes + header()->cmd_off); }

	// True when init succeeded.
	bool ok() const { return bytes != nullptr; }
};

// Encode and push one record. False if encode or push fails.
bool PushPayload(Ring& ring, std::uint16_t type, std::uint64_t id, const void* payload, std::uint32_t n) {
	Message msg;
	msg.type = type;
	msg.id = id;
	msg.size = n;
	if (n != 0) {
		std::memcpy(msg.payload, payload, n);
	}
	unsigned char buf[16 + kMaxPayload + 8];
	const std::uint32_t occ = Encode(msg, buf, sizeof(buf));
	if (occ == 0) {
		return false;
	}
	return ring.Push(buf, occ);
}

// Pop and decode one record. False if the ring is empty.
bool PopMessage(Ring& ring, Message* msg) {
	unsigned char buf[16 + kMaxPayload];
	std::uint32_t size = 0;
	if (!ring.Pop(buf, sizeof(buf), &size)) {
		return false;
	}
	return Decode(buf, size, msg);
}

// A fresh ring pops nothing, and one ping survives a push and a pop.
int TestEmpty() {
	Region region;
	if (!region.ok()) {
		return 1;
	}
	Header* header = region.header();
	if (header->magic.load(std::memory_order_acquire) != kMagic || header->version != kVersion) {
		return Fail("magic");
	}
	if (header->cmd_bytes != Ring::kCapacity || header->bulk_off != kBulkOff) {
		return Fail("offsets");
	}
	if (header->flags != 0 || header->frame.load(std::memory_order_acquire) != 0) {
		return Fail("flags");
	}
	// The halves share no byte, and the engine half can hold the demo dump.
	if (header->tool_bulk_off != header->bulk_off || (header->tool_bulk_bytes & 7u) != 0 ||
		header->ue_bulk_off != header->tool_bulk_off + header->tool_bulk_bytes ||
		header->tool_bulk_bytes + header->ue_bulk_bytes != header->bulk_bytes ||
		header->ue_bulk_bytes < 4u + 1000u * 12u) {
		return Fail("bulk split");
	}
	auto* bytes = reinterpret_cast<unsigned char*>(header);
	const float speed = 100.f;
	std::memcpy(bytes + header->tool_bulk_off, &speed, sizeof(speed));
	const std::uint32_t marker = 0xA5A5A5A5u;
	std::memcpy(bytes + header->ue_bulk_off, &marker, sizeof(marker));
	float still = 0.f;
	std::memcpy(&still, bytes + header->tool_bulk_off, sizeof(still));
	if (still != speed) {
		return Fail("halves alias");
	}
	Ring ring = region.cmd();
	Message msg;
	if (PopMessage(ring, &msg) || ring.Head() != ring.Tail()) {
		return Fail("fresh ring not empty");
	}

	PingPayload ping;
	ping.nonce = 0x1122334455667788ull;
	if (!PushPayload(ring, kPing, 1, &ping, sizeof(ping))) {
		return Fail("push ping");
	}
	if (ring.Head() == ring.Tail()) {
		return Fail("empty after push");
	}
	if (!PopMessage(ring, &msg)) {
		return Fail("pop ping");
	}
	PingPayload got;
	std::memcpy(&got, msg.payload, sizeof(got));
	if (msg.type != kPing || msg.id != 1 || msg.size != sizeof(ping) || got.nonce != ping.nonce) {
		return Fail("ping mismatch");
	}
	if (PopMessage(ring, &msg) || ring.Head() != ring.Tail()) {
		return Fail("not empty after pop");
	}
	return 0;
}

// A full ring refuses another record, including one that would skip past the end.
int TestFull() {
	Region region;
	if (!region.ok()) {
		return 1;
	}
	Ring ring = region.cmd();
	unsigned char payload[1008];
	std::memset(payload, 0x11, sizeof(payload));
	for (int i = 0; i < 64; ++i) {
		if (!PushPayload(ring, kConsole, static_cast<std::uint64_t>(i + 1), payload, sizeof(payload))) {
			return Fail("fill");
		}
	}
	if (PushPayload(ring, kConsole, 65, payload, sizeof(payload))) {
		return Fail("full accepted a record");
	}
	Message msg;
	if (!PopMessage(ring, &msg) || msg.id != 1) {
		return Fail("pop full");
	}
	if (!PushPayload(ring, kConsole, 65, payload, sizeof(payload))) {
		return Fail("push after pop");
	}

	Region gap_region;
	if (!gap_region.ok()) {
		return 1;
	}
	Ring gap = gap_region.cmd();
	for (int i = 0; i < 4095; ++i) {
		if (!PushPayload(gap, kQuit, static_cast<std::uint64_t>(i + 1), nullptr, 0)) {
			return Fail("gap fill");
		}
	}
	unsigned char wide[16];
	std::memset(wide, 0x22, sizeof(wide));
	if (PushPayload(gap, kDump, 4096, wide, sizeof(wide))) {
		return Fail("skip passed capacity");
	}
	if (!PushPayload(gap, kQuit, 4096, nullptr, 0)) {
		return Fail("exact fit rejected");
	}
	return 0;
}

// A record with no room at the end is written at offset 0 and pops intact.
int TestWrap() {
	Region region;
	if (!region.ok()) {
		return 1;
	}
	Ring ring = region.cmd();
	for (int i = 0; i < 4095; ++i) {
		if (!PushPayload(ring, kQuit, static_cast<std::uint64_t>(i + 1), nullptr, 0)) {
			return Fail("wrap fill");
		}
	}
	Message msg;
	for (int i = 0; i < 4090; ++i) {
		if (!PopMessage(ring, &msg) || msg.id != static_cast<std::uint64_t>(i + 1)) {
			return Fail("wrap drain");
		}
	}
	unsigned char payload[16];
	std::memset(payload, 0xAB, sizeof(payload));
	if (!PushPayload(ring, kSpawn, 4096, payload, sizeof(payload))) {
		return Fail("wrap push");
	}
	if ((ring.Head() & (Ring::kCapacity - 1)) != 32) {
		return Fail("wrap head");
	}

	for (int i = 4090; i < 4095; ++i) {
		if (!PopMessage(ring, &msg) || msg.id != static_cast<std::uint64_t>(i + 1) || msg.size != 0) {
			return Fail("wrap pop prefix");
		}
	}
	if (!PopMessage(ring, &msg) || msg.type != kSpawn || msg.id != 4096 || msg.size != sizeof(payload)) {
		return Fail("wrap pop record");
	}
	if (std::memcmp(msg.payload, payload, sizeof(payload)) != 0) {
		return Fail("wrap payload");
	}
	if (PopMessage(ring, &msg) || ring.Head() != ring.Tail()) {
		return Fail("wrap not empty");
	}
	return 0;
}

// 10000 messages cross two threads without a lost or reordered id.
int TestStress() {
	Region region;
	if (!region.ok()) {
		return 1;
	}
	Ring ring = region.cmd();
	// Messages exchanged on two threads. The run has to finish inside the 30 s ctest limit.
	constexpr int kCount = 10000;
	std::atomic<bool> failed{false};
	std::string error;
	std::thread consumer([&] {
		for (int i = 1; i <= kCount; ++i) {
			Message msg;
			while (!PopMessage(ring, &msg)) {
				if (failed.load(std::memory_order_acquire)) {
					return;
				}
				std::this_thread::yield();
			}
			const auto n = static_cast<std::uint32_t>((i % 64 == 0) ? kMaxPayload : (i % 513));
			if (msg.id != static_cast<std::uint64_t>(i) || msg.type != kConsole || msg.size != n) {
				error = "stress header";
				failed.store(true, std::memory_order_release);
				return;
			}
			for (std::uint32_t b = 0; b < n; ++b) {
				if (msg.payload[b] != static_cast<unsigned char>(i + b)) {
					error = "stress payload";
					failed.store(true, std::memory_order_release);
					return;
				}
			}
		}
	});

	for (int i = 1; i <= kCount && !failed.load(std::memory_order_acquire); ++i) {
		const auto n = static_cast<std::uint32_t>((i % 64 == 0) ? kMaxPayload : (i % 513));
		unsigned char payload[kMaxPayload];
		for (std::uint32_t b = 0; b < n; ++b) {
			payload[b] = static_cast<unsigned char>(i + b);
		}
		while (!PushPayload(ring, kConsole, static_cast<std::uint64_t>(i), payload, n)) {
			if (failed.load(std::memory_order_acquire)) {
				break;
			}
			std::this_thread::yield();
		}
	}
	consumer.join();
	if (!error.empty()) {
		return Fail(error.c_str());
	}
	Message extra;
	if (PopMessage(ring, &extra) || ring.Head() != ring.Tail()) {
		return Fail("stress leftover");
	}
	return 0;
}

}  // namespace

// Run the test named by argv[1]: empty, full, wrap, or stress.
int main(int argc, char** argv) {
	if (argc != 2) {
		return Fail("usage");
	}
	const std::string which = argv[1];
	if (which == "empty") {
		return TestEmpty();
	}
	if (which == "full") {
		return TestFull();
	}
	if (which == "wrap") {
		return TestWrap();
	}
	if (which == "stress") {
		return TestStress();
	}
	return Fail("usage");
}
