#include "process.h"

#include <cstdio>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

std::string Quote(const std::string& arg) {
	std::string out = "\"";
	for (char c : arg) {
		if (c == '"') {
			out += '\\';
		}
		out += c;
	}
	out += '"';
	return out;
}

}  // namespace

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
	std::string command;
	for (const std::string& arg : args) {
		if (!command.empty()) {
			command += ' ';
		}
		command += Quote(arg);
	}

	STARTUPINFOA startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION info{};
	if (!CreateProcessA(
			args[0].c_str(),
			command.data(),
			nullptr,
			nullptr,
			TRUE,
			0,
			nullptr,
			nullptr,
			&startup,
			&info)) {
		std::fprintf(stderr, "spawn %s: %lu\n", args[0].c_str(), GetLastError());
		return {};
	}
	CloseHandle(info.hThread);

	Process process;
	process.handle_ = reinterpret_cast<std::intptr_t>(info.hProcess);
	return process;
}

int Process::Wait() {
	if (handle_ == 0) {
		return -1;
	}
	HANDLE process = reinterpret_cast<HANDLE>(handle_);
	handle_ = 0;
	if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) {
		CloseHandle(process);
		return -1;
	}
	DWORD code = 1;
	GetExitCodeProcess(process, &code);
	CloseHandle(process);
	return static_cast<int>(code);
}

bool Process::TryWait(int* code) {
	if (handle_ == 0 || code == nullptr) {
		return false;
	}
	HANDLE process = reinterpret_cast<HANDLE>(handle_);
	const DWORD wait = WaitForSingleObject(process, 0);
	if (wait == WAIT_TIMEOUT) {
		return false;
	}
	if (wait != WAIT_OBJECT_0) {
		return false;
	}
	DWORD exit_code = 1;
	GetExitCodeProcess(process, &exit_code);
	CloseHandle(process);
	handle_ = 0;
	*code = static_cast<int>(exit_code);
	return true;
}

void Process::Terminate() {
	if (handle_ != 0) {
		TerminateProcess(reinterpret_cast<HANDLE>(handle_), 1);
	}
}
