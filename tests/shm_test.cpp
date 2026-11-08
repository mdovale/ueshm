#include "process.h"
#include "shm.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

namespace {

constexpr std::uint32_t kSize = 65536;
constexpr std::uint32_t kParent = 0xA1B2C3D4u;
constexpr std::uint32_t kChild = 0x11223344u;

std::string MakeName() {
	std::random_device rd;
	const auto n = static_cast<std::uint32_t>(rd());
	char buf[16];
	std::snprintf(buf, sizeof(buf), "ueshm-%08x", n);
	return buf;
}

int Fail(const char* msg) {
	std::fprintf(stderr, "%s\n", msg);
	return 1;
}

int Child(const std::string& name) {
	ShmRegion region = ShmRegion::Open(name);
	if (!region) {
		return Fail("open failed");
	}
	if (region.Size() != kSize) {
		return Fail("size mismatch");
	}
	auto* words = static_cast<std::uint32_t*>(region.Data());
	auto* bytes = static_cast<unsigned char*>(region.Data());
	if (words[0] != kParent || words[1] != 0 || bytes[kSize - 1] != 0x5A) {
		return Fail("parent bytes mismatch");
	}
	words[1] = kChild;
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc == 3 && std::strcmp(argv[1], "child") == 0) {
		return Child(argv[2]);
	}
	if (argc != 1) {
		return Fail("usage");
	}

	const std::string name = MakeName();
	{
		ShmRegion region = ShmRegion::Create(name, kSize);
		if (!region) {
			return Fail("create failed");
		}
		auto* words = static_cast<std::uint32_t*>(region.Data());
		auto* bytes = static_cast<unsigned char*>(region.Data());
		if (words[0] != 0 || words[1] != 0 || bytes[kSize - 1] != 0) {
			return Fail("not zero filled");
		}
		words[0] = kParent;
		bytes[kSize - 1] = 0x5A;

		Process child = Process::Launch({argv[0], "child", name});
		if (!child) {
			return Fail("spawn failed");
		}
		const int rc = child.Wait();
		if (rc != 0) {
			std::fprintf(stderr, "child exit %d\n", rc);
			return 1;
		}
		if (words[1] != kChild) {
			return Fail("child bytes mismatch");
		}
	}

	ShmRegion again = ShmRegion::Create(name, kSize);
	if (!again) {
		return Fail("recreate failed");
	}
	return 0;
}
