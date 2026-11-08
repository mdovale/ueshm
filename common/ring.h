#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

// SPSC byte ring. head and tail are monotonic. A record that does not fit
// in the bytes left at the end is written at offset 0, and that tail is skipped.
class Ring {
public:
	static constexpr std::uint32_t kCapacity = 65536;
	static constexpr std::uint32_t kSlot = 128;
	static constexpr std::uint32_t kBytes = kSlot * 2 + kCapacity;

	explicit Ring(void* head)
		: head_(reinterpret_cast<std::atomic<std::uint32_t>*>(head)),
		  tail_(reinterpret_cast<std::atomic<std::uint32_t>*>(
			  static_cast<unsigned char*>(head) + kSlot)),
		  data_(static_cast<unsigned char*>(head) + kSlot * 2) {}

	static void Init(void* head) {
		auto* bytes = static_cast<unsigned char*>(head);
		new (bytes) std::atomic<std::uint32_t>(0);
		new (bytes + kSlot) std::atomic<std::uint32_t>(0);
	}

	// `bytes` is the space the record occupies, a multiple of 8.
	// The first u32 of `record` is the unpadded size.
	bool Push(const void* record, std::uint32_t bytes) {
		if (record == nullptr || bytes < 16 || bytes > kCapacity || (bytes & 7u) != 0) {
			return false;
		}
		std::uint32_t size = 0;
		std::memcpy(&size, record, sizeof(size));
		if (size < 16 || Align8(size) != bytes) {
			return false;
		}

		const std::uint32_t tail = tail_->load(std::memory_order_acquire);
		std::uint32_t head = head_->load(std::memory_order_relaxed);
		const std::uint32_t pos = head & (kCapacity - 1);
		std::uint32_t gap = 0;
		if (pos != 0 && (kCapacity - pos) < bytes) {
			gap = kCapacity - pos;
		}
		if ((head - tail) + gap + bytes > kCapacity) {
			return false;
		}

		if (gap != 0) {
			if (gap >= 4) {
				const std::uint32_t zero = 0;
				std::memcpy(data_ + pos, &zero, sizeof(zero));
			}
			head += gap;
		}
		std::memcpy(data_ + (head & (kCapacity - 1)), record, bytes);
		head_->store(head + bytes, std::memory_order_release);
		return true;
	}

	// Copies the unpadded record into `dest`. Skips a wrapped tail.
	bool Pop(void* dest, std::uint32_t dest_cap, std::uint32_t* size_out) {
		if (dest == nullptr || size_out == nullptr) {
			return false;
		}
		std::uint32_t tail = tail_->load(std::memory_order_relaxed);
		for (;;) {
			const std::uint32_t head = head_->load(std::memory_order_acquire);
			if (tail == head) {
				return false;
			}
			const std::uint32_t pos = tail & (kCapacity - 1);
			const std::uint32_t remaining = kCapacity - pos;
			std::uint32_t size = 0;
			if (remaining >= 16) {
				std::memcpy(&size, data_ + pos, sizeof(size));
			}
			const std::uint32_t occ = Align8(size);
			if (size >= 16 && occ <= remaining && (head - tail) >= occ) {
				if (size > dest_cap) {
					return false;
				}
				std::memcpy(dest, data_ + pos, size);
				tail_->store(tail + occ, std::memory_order_release);
				*size_out = size;
				return true;
			}
			if (size != 0 && remaining >= 16) {
				return false;
			}
			if ((head - tail) < remaining) {
				return false;
			}
			tail += remaining;
			tail_->store(tail, std::memory_order_release);
		}
	}

	std::uint32_t Head() const { return head_->load(std::memory_order_acquire); }
	std::uint32_t Tail() const { return tail_->load(std::memory_order_acquire); }

	static constexpr std::uint32_t Align8(std::uint32_t n) { return (n + 7u) & ~7u; }

private:
	std::atomic<std::uint32_t>* head_;
	std::atomic<std::uint32_t>* tail_;
	unsigned char* data_;
};

static_assert(Ring::kCapacity == 65536, "ring data");
static_assert((Ring::kCapacity & (Ring::kCapacity - 1)) == 0, "power of two");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "lock-free u32");
static_assert(sizeof(std::atomic<std::uint32_t>) == 4, "atomic u32");
