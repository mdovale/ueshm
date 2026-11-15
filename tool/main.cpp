#include "process.h"
#include "protocol.h"
#include "shm.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

// Budget for ue_ready, and again for process exit after QUIT. Past this the run has failed.
constexpr std::chrono::seconds kReadyWait{60};
// Budget for one push or one matching reply. Separate from the ready budget.
constexpr std::chrono::seconds kReplyWait{10};

struct Options {
	std::string command;
	std::string exec;
	std::filesystem::path engine;
	std::filesystem::path project;
	std::string map = "/Engine/Maps/Entry";
	float dt = 0.f;
	std::uint32_t count = 0;
	bool sync = false;
	bool window = false;
	bool has_dt = false;
	bool has_count = false;
};

struct Session {
	ShmRegion region;
	Process process;

	// Kill a child that is still running, then mark the region closing.
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

// Print the command line to stderr.
void Usage() {
	std::fprintf(stderr,
		"usage: uectl launch|ping|demo --engine <root> --project <uproject> [--map <map>] [--sync] [--window]\n"
		"       uectl step --dt <seconds> --count <n> --engine <root> --project <uproject> [--map <map>] [--window]\n"
		"       uectl exec <command> --engine <root> --project <uproject> [--map <map>] [--sync] [--window]\n");
}

// Fill `opt` from argv. False means the caller should print usage.
bool Parse(int argc, char** argv, Options* opt) {
	if (argc < 2) {
		return false;
	}
	opt->command = argv[1];
	std::vector<std::string> extra;
	for (int i = 2; i < argc; ++i) {
		const std::string arg = argv[i];
		// Store the next argv element in `dest`. False if it is missing.
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
		} else if (arg == "--sync") {
			opt->sync = true;
		} else if (arg == "--window") {
			opt->window = true;
		} else if (arg == "--dt") {
			if (i + 1 >= argc) {
				return false;
			}
			++i;
			char* end = nullptr;
			const double parsed = std::strtod(argv[i], &end);
			if (end == argv[i] || *end != '\0') {
				return false;
			}
			opt->dt = static_cast<float>(parsed);
			opt->has_dt = true;
		} else if (arg == "--count") {
			if (i + 1 >= argc) {
				return false;
			}
			++i;
			char* end = nullptr;
			const unsigned long parsed = std::strtoul(argv[i], &end, 10);
			if (end == argv[i] || *end != '\0' || parsed == 0 || parsed > 0xfffffffful) {
				return false;
			}
			opt->count = static_cast<std::uint32_t>(parsed);
			opt->has_count = true;
		} else {
			extra.push_back(arg);
		}
	}
	if (opt->engine.empty() || opt->project.empty()) {
		return false;
	}
	if (opt->command == "exec") {
		if (opt->has_dt || opt->has_count) {
			return false;
		}
		for (const std::string& part : extra) {
			if (!opt->exec.empty()) {
				opt->exec += ' ';
			}
			opt->exec += part;
		}
		return !opt->exec.empty();
	}
	if (opt->command == "step") {
		return extra.empty() && opt->has_dt && opt->has_count;
	}
	if (opt->has_dt || opt->has_count) {
		return false;
	}
	return extra.empty() && (opt->command == "launch" || opt->command == "ping" || opt->command == "demo");
}

// First editor binary that exists under `root`, or an empty path.
// A windowed macOS launch prefers the app bundle. UnrealEditor-Cmd cannot become key.
std::filesystem::path EditorPath(const std::filesystem::path& root, bool window) {
	std::vector<std::filesystem::path> candidates;
	if (window) {
		candidates.push_back(root / "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor");
	}
	candidates.push_back(root / "Engine/Binaries/Mac/UnrealEditor-Cmd");
	candidates.push_back(root / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe");
	candidates.push_back(root / "Engine/Binaries/Linux/UnrealEditor-Cmd");
	for (const std::filesystem::path& candidate : candidates) {
		std::error_code ec;
		if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
			return candidate;
		}
	}
	return {};
}

// Logical name: ueshm- plus 8 random hex digits.
std::string MakeName() {
	std::random_device rd;
	const auto n = static_cast<std::uint32_t>(rd());
	char buf[16];
	std::snprintf(buf, sizeof(buf), "ueshm-%08x", n);
	return buf;
}

enum class Wait { Ok, Timeout, Exited };

// Sleep 10 ms until `pred`, the child exits, or `limit`. Bumps tool_heartbeat each check.
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

// Encode `msg` and push it once. False if the ring refuses the record.
bool Push(Ring& ring, const Message& msg) {
	unsigned char buf[16 + kMaxPayload + 8];
	const std::uint32_t occ = Encode(msg, buf, sizeof(buf));
	return occ != 0 && ring.Push(buf, occ);
}

// Poll until the push fits, or the reply budget expires.
bool Send(Header* header, Process* process, Ring& ring, const Message& msg) {
	const auto result = Poll(
		header, process, kReplyWait, [&] { return Push(ring, msg); }, nullptr);
	return result == Wait::Ok;
}

// Poll until a reply with `id` is popped, or the reply budget expires.
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

// Print `msg` to stderr and return 1.
int Fail(const char* msg) {
	std::fprintf(stderr, "%s\n", msg);
	return 1;
}

// After ue_ready: spawn, write the tool-half speed, step, then dump.
// Prints the stepped line. The caller sends QUIT. Nonzero on a failed check.
int RunStep(
	Header* header,
	unsigned char* base,
	Process* process,
	Ring& commands,
	Ring& replies,
	std::uint64_t* next_id,
	float dt,
	std::uint32_t count) {
	// Same actors demo spawns. step fails a short count the same way.
	constexpr std::uint32_t kCount = 1000;
	constexpr float kSpacing = 100.f;
	// Centimeters per second at the start of the tool half. The ack must echo this value.
	constexpr float kSpeed = 100.f;

	Message spawn;
	spawn.type = kSpawn;
	spawn.id = (*next_id)++;
	spawn.size = sizeof(SpawnPayload);
	SpawnPayload request;
	request.count = kCount;
	request.spacing = kSpacing;
	std::memcpy(spawn.payload, &request, sizeof(request));
	Message reply;
	if (!Send(header, process, commands, spawn) ||
		!Recv(header, process, replies, spawn.id, &reply) || reply.type != kAck ||
		reply.size != sizeof(AckPayload)) {
		return Fail("spawn failed");
	}
	AckPayload spawned;
	std::memcpy(&spawned, reply.payload, sizeof(spawned));
	if (spawned.value != kCount) {
		std::fprintf(stderr, "spawned %u\n", spawned.value);
		return 1;
	}

	// Before any STEP. The plugin reads it on the end-of-frame callback for that step.
	std::memcpy(base + header->tool_bulk_off, &kSpeed, sizeof(kSpeed));

	for (std::uint32_t n = 1; n <= count; ++n) {
		Message step;
		step.type = kStep;
		step.id = (*next_id)++;
		step.size = sizeof(StepPayload);
		StepPayload body;
		body.dt = dt;
		std::memcpy(step.payload, &body, sizeof(body));
		if (!Send(header, process, commands, step) ||
			!Recv(header, process, replies, step.id, &reply) || reply.type != kStepAck ||
			reply.size != sizeof(StepAckPayload)) {
			return Fail("step failed");
		}
		StepAckPayload ack;
		std::memcpy(&ack, reply.payload, sizeof(ack));
		if (ack.frame != n || header->frame.load(std::memory_order_acquire) != ack.frame) {
			return Fail("step frame");
		}
		if (ack.speed != kSpeed) {
			return Fail("step speed");
		}
	}

	Message dump;
	dump.type = kDump;
	dump.id = (*next_id)++;
	if (!Send(header, process, commands, dump) ||
		!Recv(header, process, replies, dump.id, &reply) || reply.type != kDone ||
		reply.size != sizeof(DonePayload)) {
		return Fail("dump failed");
	}
	DonePayload done;
	std::memcpy(&done, reply.payload, sizeof(done));
	const std::uint32_t expect_bytes = 4u + kCount * 12u;
	if (done.bytes != expect_bytes || done.bytes > header->ue_bulk_bytes) {
		return Fail("dump size");
	}
	const unsigned char* cursor = base + header->ue_bulk_off;
	std::uint32_t dumped = 0;
	std::memcpy(&dumped, cursor, sizeof(dumped));
	cursor += sizeof(dumped);
	if (dumped != kCount) {
		return Fail("dump count");
	}
	const float moved = static_cast<float>(count) * kSpeed * dt;
	for (std::uint32_t i = 0; i < dumped; ++i) {
		float xyz[3];
		std::memcpy(xyz, cursor, sizeof(xyz));
		cursor += sizeof(xyz);
		const float want = static_cast<float>(i) * kSpacing + moved;
		if (std::fabs(xyz[0] - want) > 0.01f || xyz[1] != 0.f || xyz[2] != 0.f) {
			std::fprintf(stderr, "actor %u at %g %g %g\n", i, xyz[0], xyz[1], xyz[2]);
			return 1;
		}
	}
	std::printf("stepped %u\n", count);
	return 0;
}

}  // namespace

// Run one subcommand. Nonzero on usage errors, timeout, crash, or a failed check.
int main(int argc, char** argv) {
	// Nothing is mapped yet. A bad argv line stops here.
	Options opt;
	if (!Parse(argc, argv, &opt)) {
		Usage();
		return 1;
	}

	const std::filesystem::path editor = EditorPath(opt.engine, opt.window);
	if (editor.empty()) {
		return Fail("editor not found");
	}
	// Absolute path. The editor resolves a relative .uproject from its binary directory.
	std::error_code ec;
	const std::filesystem::path project = std::filesystem::absolute(opt.project, ec);
	if (ec) {
		return Fail("project path");
	}

	// Publish the header before the child exists. The plugin waits on magic.
	// Bit 0 is stored before that release. step sets it. The other commands do too only with --sync.
	const std::string name = MakeName();
	Session session;
	session.region = ShmRegion::Create(name, kDefaultBytes);
	const std::uint32_t flags = (opt.command == "step" || opt.sync) ? kFlagSync : 0;
	if (!session.region || !InitRegion(session.region.Data(), session.region.Size(), flags)) {
		return Fail("create failed");
	}
	auto* header = reinterpret_cast<Header*>(session.region.Data());
	auto* base = static_cast<unsigned char*>(session.region.Data());
	Ring commands(base + header->cmd_off);
	Ring replies(base + header->rsp_off);

	// -ShmName is the logical name. The plugin opens that object; it does not create it.
	// --window is the only launch that can draw. Headless keeps -nullrhi, so spawn does not load a mesh.
	// The exec pins a cheat cvar. A project ini entry is ignored.
	std::vector<std::string> launch = {
		editor.string(),
		project.string(),
		opt.map,
		"-game",
	};
	if (!opt.window) {
		launch.push_back("-nullrhi");
		// Headless keeps this. A windowed Cmd launch with it never came forward.
		launch.push_back("-unattended");
	}
	launch.push_back("-nosplash");
	launch.push_back("-nopause");
	launch.push_back("-stdout");
	launch.push_back("-ExecCmds=t.IdleWhenNotForeground 0");
	launch.push_back("-ShmName=" + name);
	session.process = Process::Launch(launch);
	if (!session.process) {
		return Fail("launch failed");
	}

	// state becomes 1 only after a Game or PIE world exists.
	// Any return from here runs ~Session: kill a live child, then mark the region closing.
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
	// launch sends no orders. It prints the name and blocks until the editor exits.
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

	// demo checks the link, then a console command, then spawn, then the bulk dump.
	if (opt.command == "demo") {
		// Actors demo must spawn and read back. Any other count fails the run.
		constexpr std::uint32_t kCount = 1000;
		// Centimeters between demo actors. The position check allows 0.01 around i times this.
		constexpr float kSpacing = 100.f;
		// The nonce has to come back before spawn or the dump can be trusted.
		Message ping;
		ping.type = kPing;
		ping.id = next_id++;
		ping.size = sizeof(PingPayload);
		PingPayload nonce;
		nonce.nonce = 0x55aa55aa55aa55aaull;
		std::memcpy(ping.payload, &nonce, sizeof(nonce));
		Message reply;
		if (!Send(header, &session.process, commands, ping) ||
			!Recv(header, &session.process, replies, ping.id, &reply) || reply.type != kPong ||
			reply.size != sizeof(PingPayload)) {
			return Fail("ping failed");
		}
		PingPayload got;
		std::memcpy(&got, reply.payload, sizeof(got));
		if (got.nonce != nonce.nonce) {
			return Fail("ping nonce");
		}

		// ACK value 1 means Exec accepted the command, not merely that a reply arrived.
		const char* console = "stat fps";
		Message exec;
		exec.type = kConsole;
		exec.id = next_id++;
		exec.size = static_cast<std::uint32_t>(std::strlen(console));
		std::memcpy(exec.payload, console, exec.size);
		if (!Send(header, &session.process, commands, exec) ||
			!Recv(header, &session.process, replies, exec.id, &reply) || reply.type != kAck ||
			reply.size != sizeof(AckPayload)) {
			return Fail("exec failed");
		}
		AckPayload console_ack;
		std::memcpy(&console_ack, reply.payload, sizeof(console_ack));
		if (console_ack.value != 1u) {
			return Fail("exec rejected");
		}

		// A short count fails the run. demo does not accept a partial spawn.
		Message spawn;
		spawn.type = kSpawn;
		spawn.id = next_id++;
		spawn.size = sizeof(SpawnPayload);
		SpawnPayload request;
		request.count = kCount;
		request.spacing = kSpacing;
		std::memcpy(spawn.payload, &request, sizeof(request));
		if (!Send(header, &session.process, commands, spawn) ||
			!Recv(header, &session.process, replies, spawn.id, &reply) || reply.type != kAck ||
			reply.size != sizeof(AckPayload)) {
			return Fail("spawn failed");
		}
		AckPayload spawned;
		std::memcpy(&spawned, reply.payload, sizeof(spawned));
		if (spawned.value != kCount) {
			std::fprintf(stderr, "spawned %u\n", spawned.value);
			return 1;
		}

		// Read positions only after DONE. That reply is what publishes the bulk bytes.
		Message dump;
		dump.type = kDump;
		dump.id = next_id++;
		if (!Send(header, &session.process, commands, dump) ||
			!Recv(header, &session.process, replies, dump.id, &reply) || reply.type != kDone ||
			reply.size != sizeof(DonePayload)) {
			return Fail("dump failed");
		}
		DonePayload done;
		std::memcpy(&done, reply.payload, sizeof(done));
		const std::uint32_t expect_bytes = 4u + kCount * 12u;
		if (done.bytes != expect_bytes || done.bytes > header->ue_bulk_bytes) {
			return Fail("dump size");
		}
		// Engine half only. demo does not write the tool half.
		const unsigned char* cursor = base + header->ue_bulk_off;
		std::uint32_t count = 0;
		std::memcpy(&count, cursor, sizeof(count));
		cursor += sizeof(count);
		if (count != kCount) {
			return Fail("dump count");
		}
		for (std::uint32_t i = 0; i < count; ++i) {
			float xyz[3];
			std::memcpy(xyz, cursor, sizeof(xyz));
			cursor += sizeof(xyz);
			const float want = static_cast<float>(i) * kSpacing;
			if (std::fabs(xyz[0] - want) > 0.01f || xyz[1] != 0.f || xyz[2] != 0.f) {
				std::fprintf(stderr, "actor %u at %g %g %g\n", i, xyz[0], xyz[1], xyz[2]);
				return 1;
			}
		}
		std::printf("spawned %u\n", spawned.value);
	} else if (opt.command == "ping") {
		// Same nonce as demo. The printed value is the one the plugin echoed.
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
	} else if (opt.command == "step") {
		const int step_rc = RunStep(
			header, base, &session.process, commands, replies, &next_id, opt.dt, opt.count);
		if (step_rc != 0) {
			return step_rc;
		}
	} else {
		// exec. The joined arguments are the raw CONSOLE payload.
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

	// ping, exec, and demo ask the editor to quit, then wait for exit code 0.
	// launch returned above and does not reach this.
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
