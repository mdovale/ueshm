#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Process {
public:
	// No child.
	Process() = default;
	Process(const Process&) = delete;
	Process& operator=(const Process&) = delete;
	// Take `other`'s child. `other` no longer owns it.
	Process(Process&& other) noexcept;
	// Wait for this child, then take `other`'s.
	Process& operator=(Process&& other) noexcept;
	// Wait for the child if one is still owned. Does not kill it.
	~Process();

	// Start `args[0]` with the rest of `args`. Empty if the spawn fails.
	static Process Launch(const std::vector<std::string>& args);
	// Block until exit. The status, or -1 if no child is owned.
	int Wait();
	// True once the process has exited. Leaves a running process alone.
	bool TryWait(int* code);
	// Kill the child. Does not wait.
	void Terminate();

	// True when a child is owned.
	explicit operator bool() const { return handle_ != 0; }

private:
	std::intptr_t handle_ = 0;
};
