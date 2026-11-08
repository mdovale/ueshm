#include "shm.h"

#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

std::string ObjectName(const std::string& logical) {
	return "Local\\" + logical;
}

void Fail(const char* what, const std::string& name, unsigned long err) {
	std::fprintf(stderr, "shm %s %s: %lu\n", what, name.c_str(), err);
}

// NtQuerySection(SectionBasicInformation) reports the mapping size.
// The ULONG before MaximumSize is padded to 8 bytes on Win64.
struct SectionBasic {
	void* base;
	unsigned long attributes;
	unsigned long pad;
	long long size;
};
static_assert(sizeof(void*) == 8, "Win64");
static_assert(sizeof(SectionBasic) == 24, "section basic");

std::uint32_t MappingSize(HANDLE handle, void* view) {
	using NtQuerySectionFn = long(__stdcall*)(void*, unsigned long, void*, unsigned long, unsigned long*);
	auto query = reinterpret_cast<NtQuerySectionFn>(
		GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySection"));
	if (query != nullptr) {
		SectionBasic info{};
		const long status = query(handle, 0, &info, sizeof(info), nullptr);
		if (status >= 0 && info.size > 0 && info.size <= 0xffffffffLL) {
			return static_cast<std::uint32_t>(info.size);
		}
	}
	MEMORY_BASIC_INFORMATION mem{};
	if (VirtualQuery(view, &mem, sizeof(mem)) != 0 && mem.RegionSize > 0 &&
		mem.RegionSize <= 0xffffffffu) {
		return static_cast<std::uint32_t>(mem.RegionSize);
	}
	return 0;
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
		UnmapViewOfFile(data_);
		data_ = nullptr;
	}
	if (handle_ != -1) {
		CloseHandle(reinterpret_cast<HANDLE>(handle_));
		handle_ = -1;
	}
	owner_ = false;
	name_.clear();
	size_ = 0;
}

ShmRegion ShmRegion::Create(const std::string& name, std::uint32_t size) {
	if (name.empty() || size == 0) {
		return {};
	}
	const std::string object = ObjectName(name);
	SetLastError(0);
	HANDLE handle = CreateFileMappingA(
		INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, object.c_str());
	if (handle == nullptr) {
		Fail("create", object, GetLastError());
		return {};
	}
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		Fail("create", object, ERROR_ALREADY_EXISTS);
		CloseHandle(handle);
		return {};
	}
	void* data = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
	if (data == nullptr) {
		Fail("map", object, GetLastError());
		CloseHandle(handle);
		return {};
	}

	ShmRegion region;
	region.data_ = data;
	region.size_ = size;
	region.handle_ = reinterpret_cast<std::intptr_t>(handle);
	region.owner_ = true;
	region.name_ = object;
	return region;
}

ShmRegion ShmRegion::Open(const std::string& name) {
	if (name.empty()) {
		return {};
	}
	const std::string object = ObjectName(name);
	HANDLE handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, object.c_str());
	if (handle == nullptr) {
		Fail("open", object, GetLastError());
		return {};
	}
	void* data = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (data == nullptr) {
		Fail("map", object, GetLastError());
		CloseHandle(handle);
		return {};
	}
	const std::uint32_t size = MappingSize(handle, data);
	if (size == 0) {
		Fail("size", object, GetLastError());
		UnmapViewOfFile(data);
		CloseHandle(handle);
		return {};
	}

	ShmRegion region;
	region.data_ = data;
	region.size_ = size;
	region.handle_ = reinterpret_cast<std::intptr_t>(handle);
	region.owner_ = false;
	return region;
}
