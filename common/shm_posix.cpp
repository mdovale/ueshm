#include "shm.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string ObjectName(const std::string& logical) {
	return "/" + logical;
}

void Fail(const char* what, const std::string& name) {
	std::fprintf(stderr, "shm %s %s: %s\n", what, name.c_str(), std::strerror(errno));
}

}  // namespace

ShmRegion::ShmRegion(ShmRegion&& other) noexcept
	: data_(other.data_),
	  size_(other.size_),
	  handle_(other.handle_),
	  owner_(other.owner_),
	  name_(std::move(other.name_)) {
	other.data_ = nullptr;
	other.size_ = 0;
	other.handle_ = -1;
	other.owner_ = false;
}

ShmRegion& ShmRegion::operator=(ShmRegion&& other) noexcept {
	if (this != &other) {
		Close();
		data_ = other.data_;
		size_ = other.size_;
		handle_ = other.handle_;
		owner_ = other.owner_;
		name_ = std::move(other.name_);
		other.data_ = nullptr;
		other.size_ = 0;
		other.handle_ = -1;
		other.owner_ = false;
	}
	return *this;
}

ShmRegion::~ShmRegion() { Close(); }

void ShmRegion::Close() {
	if (data_ != nullptr) {
		munmap(data_, size_);
		data_ = nullptr;
	}
	if (handle_ >= 0) {
		close(static_cast<int>(handle_));
		handle_ = -1;
	}
	if (owner_) {
		shm_unlink(name_.c_str());
		owner_ = false;
	}
	name_.clear();
	size_ = 0;
}

ShmRegion ShmRegion::Create(const std::string& name, std::uint32_t size) {
	if (name.empty() || size == 0) {
		return {};
	}
	const std::string object = ObjectName(name);
	// macOS rejects shm names longer than 31 bytes, including the slash.
	if (object.size() > 31) {
		std::fprintf(stderr, "shm name too long: %s\n", object.c_str());
		return {};
	}

	shm_unlink(object.c_str());
	const int fd = shm_open(object.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0) {
		Fail("create", object);
		return {};
	}
	if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
		Fail("truncate", object);
		close(fd);
		shm_unlink(object.c_str());
		return {};
	}
	void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		Fail("map", object);
		close(fd);
		shm_unlink(object.c_str());
		return {};
	}

	ShmRegion region;
	region.data_ = data;
	region.size_ = size;
	region.handle_ = fd;
	region.owner_ = true;
	region.name_ = object;
	return region;
}

ShmRegion ShmRegion::Open(const std::string& name) {
	if (name.empty()) {
		return {};
	}
	const std::string object = ObjectName(name);
	const int fd = shm_open(object.c_str(), O_RDWR, 0600);
	if (fd < 0) {
		Fail("open", object);
		return {};
	}
	struct stat st {};
	if (fstat(fd, &st) != 0 || st.st_size <= 0) {
		Fail("stat", object);
		close(fd);
		return {};
	}
	const auto size = static_cast<std::uint32_t>(st.st_size);
	void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		Fail("map", object);
		close(fd);
		return {};
	}

	ShmRegion region;
	region.data_ = data;
	region.size_ = size;
	region.handle_ = fd;
	region.owner_ = false;
	return region;
}
