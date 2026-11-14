#pragma once

#include "ring.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

// ASCII "UESH". The plugin treats any other value as an unpublished header.
constexpr std::uint32_t kMagic = 0x48534555;
// Layout id. Any other value leaves state at 0, so the tool hits the ready timeout.
constexpr std::uint32_t kVersion = 1;
// Both sides map the header as this many bytes. The command ring starts on the next byte.
constexpr std::uint32_t kHeaderBytes = 128;
// Create size. Must cover the header, both rings, and a bulk dump of 1000 positions.
constexpr std::uint32_t kDefaultBytes = 16u * 1024u * 1024u;
// Largest payload a ring record may carry. Larger results belong in the bulk area.
constexpr std::uint32_t kMaxPayload = 1024;

// Header is published and no game world exists yet. The tool must not send.
constexpr std::uint32_t kStateCreated = 0;
// A Game or PIE world exists. The tool may send.
constexpr std::uint32_t kStateUeReady = 1;
// The tool is dropping the mapping. The plugin must stop using it.
constexpr std::uint32_t kStateClosing = 2;

// Request type. Payload is exactly one u64 nonce; any other size falls through to ERR.
constexpr std::uint16_t kPing = 1;
// Request type. Payload is raw UTF-8 with no trailing NUL, at most kMaxPayload bytes.
constexpr std::uint16_t kConsole = 2;
// Request type. Payload is exactly one u32 count and one f32 spacing.
constexpr std::uint16_t kSpawn = 3;
// Request type. Payload is empty. A non-empty payload falls through to ERR.
constexpr std::uint16_t kDump = 4;
// Request type. Payload is empty. After its ACK the handler stops the drain.
constexpr std::uint16_t kQuit = 5;
// Reply type, above the request range. Payload is the nonce from the PING it answers.
constexpr std::uint16_t kPong = 101;
// Reply type, above the request range. Payload is one u32, except QUIT, which sends none.
constexpr std::uint16_t kAck = 102;
// Reply type, above the request range. Payload is the bulk byte count. Read bulk only after this.
constexpr std::uint16_t kDone = 103;
// Reply type, above the request range. Payload is the request type that failed, under the same id.
constexpr std::uint16_t kErr = 104;

// First byte of the command ring. Equal to the header size, so the two do not overlap.
constexpr std::uint32_t kCmdOff = kHeaderBytes;
// First byte of the reply ring. One full ring after the command ring.
constexpr std::uint32_t kRspOff = kHeaderBytes + Ring::kBytes;
// First bulk byte. One full ring after the reply ring. A shorter mapping is rejected.
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

// Zero the mapping, fill the header and both rings, then release-store magic.
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

// Write one ring record and its trailing pad. Returns the padded length, or 0 if it does not fit.
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

// Fill `out` from a popped record. False when the prefix disagrees with `record_size`.
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
