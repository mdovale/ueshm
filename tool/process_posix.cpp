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

Process::Process(Process&& other) noexcept : handle_(other.handle_) {
	other.handle_ = 0;
}

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

Process::~Process() {
	if (handle_ != 0) {
		Wait();
	}
}

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
	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	if (WIFSIGNALED(status)) {
		return 128 + WTERMSIG(status);
	}
	return -1;
}

void Process::Terminate() {
	if (handle_ != 0) {
		kill(static_cast<pid_t>(handle_), SIGKILL);
	}
}
