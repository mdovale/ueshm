#pragma once

#include <cstdint>
#include <string>

// Logical name is ueshm-<8 hex>. The POSIX object is /<name>.
// The Windows mapping is Local\<name>.
class ShmRegion {
public:
	ShmRegion() = default;
	ShmRegion(const ShmRegion&) = delete;
	ShmRegion& operator=(const ShmRegion&) = delete;
	ShmRegion(ShmRegion&& other) noexcept;
	ShmRegion& operator=(ShmRegion&& other) noexcept;
	~ShmRegion();

	static ShmRegion Create(const std::string& name, std::uint32_t size);
	static ShmRegion Open(const std::string& name);

	explicit operator bool() const { return data_ != nullptr; }
	void* Data() const { return data_; }
	std::uint32_t Size() const { return size_; }

private:
	void Close();

	void* data_ = nullptr;
	std::uint32_t size_ = 0;
	std::intptr_t handle_ = -1;
	bool owner_ = false;
	std::string name_;
};
