#pragma once

#include <cstdint>
#include <string>

// Logical name is ueshm-<8 hex>. The POSIX object is /<name>.
// The Windows mapping is Local\<name>.
class ShmRegion {
public:
	// Not mapped.
	ShmRegion() = default;
	ShmRegion(const ShmRegion&) = delete;
	ShmRegion& operator=(const ShmRegion&) = delete;
	// Take `other`'s mapping. `other` is left empty.
	ShmRegion(ShmRegion&& other) noexcept;
	// Close this mapping, then take `other`'s.
	ShmRegion& operator=(ShmRegion&& other) noexcept;
	// Unmap. The creator also removes the object name.
	~ShmRegion();

	// Zero-filled mapping of `size` bytes. Empty if the name is in use or the call fails.
	static ShmRegion Create(const std::string& name, std::uint32_t size);
	// Map an existing object. Does not remove the name. Empty on failure.
	static ShmRegion Open(const std::string& name);

	// True when the mapping is open.
	explicit operator bool() const { return data_ != nullptr; }
	// Base address, or null.
	void* Data() const { return data_; }
	// Mapped bytes, or 0.
	std::uint32_t Size() const { return size_; }

private:
	// Unmap, and remove the name if this side created the object.
	void Close();

	void* data_ = nullptr;
	std::uint32_t size_ = 0;
	std::intptr_t handle_ = -1;
	bool owner_ = false;
	std::string name_;
};
