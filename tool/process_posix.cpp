#include "process.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

// Take `other`'s child. `other` no longer owns it.
Process::Process(Process&& other) noexcept : handle_(other.handle_) {
	other.handle_ = 0;
}

// Wait for this child, then take `other`'s.
Process& Process::operator=(Process&& other) noexcept {
	if (this != &other) {
		if (handle_ != 0) {
			Wait();
		}
		handle_ = other.handle_;
		other.handle_ = 0;
	}
	return *this;
}

// Wait for the child if one is still owned. Does not kill it.
Process::~Process() {
	if (handle_ != 0) {
		Wait();
	}
}

// Start `args[0]` with the rest of `args`. Empty if the spawn fails.
Process Process::Launch(const std::vector<std::string>& args) {
	if (args.empty()) {
		return {};
	}
	std::vector<char*> argv;
	argv.reserve(args.size() + 1);
	for (const std::string& arg : args) {
		argv.push_back(const_cast<char*>(arg.c_str()));
	}
	argv.push_back(nullptr);

	pid_t pid = 0;
	const int rc = posix_spawn(&pid, args[0].c_str(), nullptr, nullptr, argv.data(), environ);
	if (rc != 0) {
		std::fprintf(stderr, "spawn %s: %s\n", args[0].c_str(), std::strerror(rc));
		return {};
	}

	Process process;
	process.handle_ = static_cast<std::intptr_t>(pid);
	return process;
}

// Exit status, or 128 plus the signal if the child was killed.
int StatusCode(int status) {
	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	if (WIFSIGNALED(status)) {
		return 128 + WTERMSIG(status);
	}
	return -1;
}

// Block until exit. The status, or -1 if no child is owned.
int Process::Wait() {
	if (handle_ == 0) {
		return -1;
	}
	int status = 0;
	const pid_t pid = static_cast<pid_t>(handle_);
	handle_ = 0;
	if (waitpid(pid, &status, 0) < 0) {
		return -1;
	}
	return StatusCode(status);
}

// True once the process has exited. Leaves a running process alone.
bool Process::TryWait(int* code) {
	if (handle_ == 0 || code == nullptr) {
		return false;
	}
	int status = 0;
	const pid_t pid = static_cast<pid_t>(handle_);
	const pid_t got = waitpid(pid, &status, WNOHANG);
	if (got == 0) {
		return false;
	}
	if (got < 0) {
		return false;
	}
	handle_ = 0;
	*code = StatusCode(status);
	return true;
}

// Kill the child. Does not wait.
void Process::Terminate() {
	if (handle_ != 0) {
		kill(static_cast<pid_t>(handle_), SIGKILL);
	}
}
