#pragma once

#include "ring.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

constexpr std::uint32_t kMagic = 0x48534555;
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kHeaderBytes = 128;
constexpr std::uint32_t kDefaultBytes = 16u * 1024u * 1024u;
constexpr std::uint32_t kMaxPayload = 1024;

constexpr std::uint32_t kStateCreated = 0;
constexpr std::uint32_t kStateUeReady = 1;
constexpr std::uint32_t kStateClosing = 2;

constexpr std::uint16_t kPing = 1;
constexpr std::uint16_t kConsole = 2;
constexpr std::uint16_t kSpawn = 3;
constexpr std::uint16_t kDump = 4;
constexpr std::uint16_t kQuit = 5;
constexpr std::uint16_t kPong = 101;
constexpr std::uint16_t kAck = 102;
constexpr std::uint16_t kDone = 103;
constexpr std::uint16_t kErr = 104;

constexpr std::uint32_t kCmdOff = kHeaderBytes;
constexpr std::uint32_t kRspOff = kHeaderBytes + Ring::kBytes;
constexpr std::uint32_t kBulkOff = kRspOff + Ring::kBytes;

struct Header {
	std::atomic<std::uint32_t> magic;
	std::uint32_t version;
	std::uint32_t total_size;
	std::atomic<std::uint32_t> state;
	std::atomic<std::uint32_t> tool_heartbeat;
	std::atomic<std::uint32_t> ue_heartbeat;
	std::uint32_t cmd_off;
	std::uint32_t cmd_bytes;
	std::uint32_t rsp_off;
	std::uint32_t rsp_bytes;
	std::uint32_t bulk_off;
	std::uint32_t bulk_bytes;
	unsigned char pad[80];
};

static_assert(sizeof(Header) == kHeaderBytes, "header pad");
static_assert(offsetof(Header, magic) == 0, "magic");
static_assert(offsetof(Header, version) == 4, "version");
static_assert(offsetof(Header, total_size) == 8, "total_size");
static_assert(offsetof(Header, state) == 12, "state");
static_assert(offsetof(Header, tool_heartbeat) == 16, "tool_heartbeat");
static_assert(offsetof(Header, ue_heartbeat) == 20, "ue_heartbeat");
static_assert(offsetof(Header, cmd_off) == 24, "cmd_off");
static_assert(offsetof(Header, cmd_bytes) == 28, "cmd_bytes");
static_assert(offsetof(Header, rsp_off) == 32, "rsp_off");
static_assert(offsetof(Header, rsp_bytes) == 36, "rsp_bytes");
static_assert(offsetof(Header, bulk_off) == 40, "bulk_off");
static_assert(offsetof(Header, bulk_bytes) == 44, "bulk_bytes");
static_assert(offsetof(Header, pad) == 48, "pad");
static_assert(kCmdOff == 128, "cmd_off");
static_assert(kRspOff == 65920, "rsp_off");
static_assert(kBulkOff == 131712, "bulk_off");

struct Prefix {
	std::uint32_t size;
	std::uint16_t type;
	std::uint16_t pad;
	std::uint64_t id;
};

static_assert(sizeof(Prefix) == 16, "prefix");
static_assert(offsetof(Prefix, type) == 4, "type");
static_assert(offsetof(Prefix, pad) == 6, "pad");
static_assert(offsetof(Prefix, id) == 8, "id");

struct PingPayload {
	std::uint64_t nonce;
};
struct AckPayload {
	std::uint32_t value;
};
struct DonePayload {
	std::uint32_t bytes;
};
struct ErrPayload {
	std::uint32_t type;
};
struct SpawnPayload {
	std::uint32_t count;
	float spacing;
};

static_assert(sizeof(PingPayload) == 8, "ping");
static_assert(sizeof(AckPayload) == 4, "ack");
static_assert(sizeof(DonePayload) == 4, "done");
static_assert(sizeof(ErrPayload) == 4, "err");
static_assert(sizeof(SpawnPayload) == 8, "spawn");
static_assert(offsetof(SpawnPayload, spacing) == 4, "spacing");

struct Message {
	std::uint16_t type = 0;
	std::uint64_t id = 0;
	std::uint32_t size = 0;
	unsigned char payload[kMaxPayload]{};
};

inline bool InitRegion(void* base, std::uint32_t total_size) {
	if (base == nullptr || total_size < kBulkOff) {
		return false;
	}
	auto* bytes = static_cast<unsigned char*>(base);
	std::memset(bytes, 0, total_size);

	auto* header = reinterpret_cast<Header*>(bytes);
	new (&header->magic) std::atomic<std::uint32_t>(0);
	new (&header->state) std::atomic<std::uint32_t>(kStateCreated);
	new (&header->tool_heartbeat) std::atomic<std::uint32_t>(0);
	new (&header->ue_heartbeat) std::atomic<std::uint32_t>(0);
	header->version = kVersion;
	header->total_size = total_size;
	header->cmd_off = kCmdOff;
	header->cmd_bytes = Ring::kCapacity;
	header->rsp_off = kRspOff;
	header->rsp_bytes = Ring::kCapacity;
	header->bulk_off = kBulkOff;
	header->bulk_bytes = total_size - kBulkOff;
	Ring::Init(bytes + kCmdOff);
	Ring::Init(bytes + kRspOff);
	header->magic.store(kMagic, std::memory_order_release);
	return true;
}

inline std::uint32_t Encode(const Message& msg, unsigned char* dest, std::uint32_t cap) {
	if (dest == nullptr || msg.size > kMaxPayload) {
		return 0;
	}
	const std::uint32_t size = 16u + msg.size;
	const std::uint32_t occ = Ring::Align8(size);
	if (occ > cap) {
		return 0;
	}
	std::memset(dest, 0, occ);
	Prefix prefix;
	prefix.size = size;
	prefix.type = msg.type;
	prefix.pad = 0;
	prefix.id = msg.id;
	std::memcpy(dest, &prefix, sizeof(prefix));
	if (msg.size != 0) {
		std::memcpy(dest + sizeof(prefix), msg.payload, msg.size);
	}
	return occ;
}

inline bool Decode(const unsigned char* record, std::uint32_t record_size, Message* out) {
	if (record == nullptr || out == nullptr || record_size < sizeof(Prefix)) {
		return false;
	}
	Prefix prefix;
	std::memcpy(&prefix, record, sizeof(prefix));
	if (prefix.pad != 0 || prefix.size < sizeof(Prefix) || prefix.size > record_size) {
		return false;
	}
	const std::uint32_t payload = prefix.size - static_cast<std::uint32_t>(sizeof(Prefix));
	if (payload > kMaxPayload) {
		return false;
	}
	out->type = prefix.type;
	out->id = prefix.id;
	out->size = payload;
	if (payload != 0) {
		std::memcpy(out->payload, record + sizeof(Prefix), payload);
	}
	return true;
}
