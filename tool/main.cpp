#include "process.h"
#include "protocol.h"
#include "shm.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::chrono::seconds kReadyWait{60};
constexpr std::chrono::seconds kReplyWait{10};

struct Options {
	std::string command;
	std::string exec;
	std::filesystem::path engine;
	std::filesystem::path project;
	std::string map = "/Engine/Maps/Entry";
};

struct Session {
	ShmRegion region;
	Process process;

	~Session()
	{
		if (process) {
			process.Terminate();
			process.Wait();
		}
		if (region) {
			reinterpret_cast<Header*>(region.Data())->state.store(kStateClosing, std::memory_order_release);
		}
	}
};

void Usage() {
	std::fprintf(stderr,
		"usage: uectl launch|ping|exec --engine <root> --project <uproject> [--map <map>]\n"
		"       uectl exec <command> --engine <root> --project <uproject> [--map <map>]\n");
}

bool Parse(int argc, char** argv, Options* opt) {
	if (argc < 2) {
		return false;
	}
	opt->command = argv[1];
	std::vector<std::string> extra;
	for (int i = 2; i < argc; ++i) {
		const std::string arg = argv[i];
		auto take = [&](std::filesystem::path* dest) {
			if (i + 1 >= argc) {
				return false;
			}
			*dest = argv[++i];
			return true;
		};
		if (arg == "--engine") {
			if (!take(&opt->engine)) {
				return false;
			}
		} else if (arg == "--project") {
			if (!take(&opt->project)) {
				return false;
			}
		} else if (arg == "--map") {
			if (i + 1 >= argc) {
				return false;
			}
			opt->map = argv[++i];
		} else {
			extra.push_back(arg);
		}
	}
	if (opt->engine.empty() || opt->project.empty()) {
		return false;
	}
	if (opt->command == "exec") {
		for (const std::string& part : extra) {
			if (!opt->exec.empty()) {
				opt->exec += ' ';
			}
			opt->exec += part;
		}
		return !opt->exec.empty();
	}
	return extra.empty() && (opt->command == "launch" || opt->command == "ping");
}

std::filesystem::path EditorPath(const std::filesystem::path& root) {
	const std::filesystem::path candidates[] = {
		root / "Engine/Binaries/Mac/UnrealEditor-Cmd",
		root / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe",
		root / "Engine/Binaries/Linux/UnrealEditor-Cmd",
	};
	for (const std::filesystem::path& candidate : candidates) {
		std::error_code ec;
		if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
			return candidate;
		}
	}
	return {};
}

std::string MakeName() {
	std::random_device rd;
	const auto n = static_cast<std::uint32_t>(rd());
	char buf[16];
	std::snprintf(buf, sizeof(buf), "ueshm-%08x", n);
	return buf;
}

enum class Wait { Ok, Timeout, Exited };

template <class Pred>
Wait Poll(Header* header, Process* process, std::chrono::milliseconds limit, Pred pred, int* exit_code) {
	const auto deadline = std::chrono::steady_clock::now() + limit;
	for (;;) {
		header->tool_heartbeat.fetch_add(1, std::memory_order_relaxed);
		if (pred()) {
			return Wait::Ok;
		}
		int code = 0;
		if (process->TryWait(&code)) {
			if (exit_code) {
				*exit_code = code;
			}
			return Wait::Exited;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return Wait::Timeout;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

bool Push(Ring& ring, const Message& msg) {
	unsigned char buf[16 + kMaxPayload + 8];
	const std::uint32_t occ = Encode(msg, buf, sizeof(buf));
	return occ != 0 && ring.Push(buf, occ);
}

bool Send(Header* header, Process* process, Ring& ring, const Message& msg) {
	const auto result = Poll(
		header, process, kReplyWait, [&] { return Push(ring, msg); }, nullptr);
	return result == Wait::Ok;
}

bool Recv(Header* header, Process* process, Ring& ring, std::uint64_t id, Message* reply) {
	const auto result = Poll(
		header,
		process,
		kReplyWait,
		[&] {
			unsigned char buf[16 + kMaxPayload];
			std::uint32_t size = 0;
			if (!ring.Pop(buf, sizeof(buf), &size)) {
				return false;
			}
			return Decode(buf, size, reply) && reply->id == id;
		},
		nullptr);
	return result == Wait::Ok;
}

int Fail(const char* msg) {
	std::fprintf(stderr, "%s\n", msg);
	return 1;
}

}  // namespace

int main(int argc, char** argv) {
	Options opt;
	if (!Parse(argc, argv, &opt)) {
		Usage();
		return 1;
	}

	const std::filesystem::path editor = EditorPath(opt.engine);
	if (editor.empty()) {
		return Fail("editor not found");
	}
	std::error_code ec;
	const std::filesystem::path project = std::filesystem::absolute(opt.project, ec);
	if (ec) {
		return Fail("project path");
	}

	const std::string name = MakeName();
	Session session;
	session.region = ShmRegion::Create(name, kDefaultBytes);
	if (!session.region || !InitRegion(session.region.Data(), session.region.Size())) {
		return Fail("create failed");
	}
	auto* header = reinterpret_cast<Header*>(session.region.Data());
	auto* base = static_cast<unsigned char*>(session.region.Data());
	Ring commands(base + header->cmd_off);
	Ring replies(base + header->rsp_off);

	session.process = Process::Launch({
		editor.string(),
		project.string(),
		opt.map,
		"-game",
		"-nullrhi",
		"-unattended",
		"-nosplash",
		"-nopause",
		"-stdout",
		"-ShmName=" + name,
	});
	if (!session.process) {
		return Fail("launch failed");
	}

	int exit_code = 0;
	const Wait ready = Poll(
		header,
		&session.process,
		kReadyWait,
		[&] { return header->state.load(std::memory_order_acquire) == kStateUeReady; },
		&exit_code);
	if (ready == Wait::Timeout) {
		return Fail("ready timeout");
	}
	if (ready == Wait::Exited) {
		std::fprintf(stderr, "editor exited %d\n", exit_code);
		return 1;
	}

	std::uint64_t next_id = 1;
	if (opt.command == "launch") {
		std::printf("%s\n", name.c_str());
		std::fflush(stdout);
		for (;;) {
			header->tool_heartbeat.fetch_add(1, std::memory_order_relaxed);
			if (session.process.TryWait(&exit_code)) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (exit_code != 0) {
			std::fprintf(stderr, "editor exited %d\n", exit_code);
			return 1;
		}
		return 0;
	}

	if (opt.command == "ping") {
		Message msg;
		msg.type = kPing;
		msg.id = next_id++;
		msg.size = sizeof(PingPayload);
		PingPayload nonce;
		nonce.nonce = 0x55aa55aa55aa55aaull;
		std::memcpy(msg.payload, &nonce, sizeof(nonce));
		Message reply;
		if (!Send(header, &session.process, commands, msg) ||
			!Recv(header, &session.process, replies, msg.id, &reply) || reply.type != kPong ||
			reply.size != sizeof(PingPayload)) {
			return Fail("ping failed");
		}
		PingPayload got;
		std::memcpy(&got, reply.payload, sizeof(got));
		if (got.nonce != nonce.nonce) {
			return Fail("ping nonce");
		}
		std::printf("pong %llx\n", static_cast<unsigned long long>(got.nonce));
	} else {
		if (opt.exec.size() > kMaxPayload) {
			return Fail("command too long");
		}
		Message msg;
		msg.type = kConsole;
		msg.id = next_id++;
		msg.size = static_cast<std::uint32_t>(opt.exec.size());
		std::memcpy(msg.payload, opt.exec.data(), opt.exec.size());
		Message reply;
		if (!Send(header, &session.process, commands, msg) ||
			!Recv(header, &session.process, replies, msg.id, &reply) || reply.type != kAck ||
			reply.size != sizeof(AckPayload)) {
			return Fail("exec failed");
		}
		AckPayload ack;
		std::memcpy(&ack, reply.payload, sizeof(ack));
		if (ack.value != 1u) {
			return Fail("exec rejected");
		}
		std::printf("ok %u\n", ack.value);
	}

	{
		Message quit;
		quit.type = kQuit;
		quit.id = next_id++;
		Message ack;
		if (!Send(header, &session.process, commands, quit) ||
			!Recv(header, &session.process, replies, quit.id, &ack) || ack.type != kAck) {
			return Fail("quit failed");
		}
	}

	const Wait gone = Poll(
		header,
		&session.process,
		kReadyWait,
		[] { return false; },
		&exit_code);
	if (gone != Wait::Exited) {
		return Fail("editor hung");
	}
	if (exit_code != 0) {
		std::fprintf(stderr, "editor exited %d\n", exit_code);
		return 1;
	}
	return 0;
}
