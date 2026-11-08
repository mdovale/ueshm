#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Process {
public:
	Process() = default;
	Process(const Process&) = delete;
	Process& operator=(const Process&) = delete;
	Process(Process&& other) noexcept;
	Process& operator=(Process&& other) noexcept;
	~Process();

	static Process Launch(const std::vector<std::string>& args);
	int Wait();
	// True once the process has exited. Leaves a running process alone.
	bool TryWait(int* code);
	void Terminate();

	explicit operator bool() const { return handle_ != 0; }

private:
	std::intptr_t handle_ = 0;
};
