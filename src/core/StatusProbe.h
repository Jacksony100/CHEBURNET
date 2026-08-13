#pragma once
#include <optional>
#include <string>
#include <vector>

namespace cheburnet::probe {

enum class ServiceState { NotInstalled, Stopped, Running, StopPending, Other };

struct ProcEntry {
    unsigned long pid = 0;
    std::wstring  exeName;
};

// Query a Windows service by name.
ServiceState QueryService(const std::wstring& name);

// True if the service is Running or StopPending (i.e. actively holding state).
bool ServiceActive(const std::wstring& name);

// All running processes whose image name equals exeName (case-insensitive).
std::vector<ProcEntry> FindProcesses(const std::wstring& exeName);

// Full image path of a pid, or nullopt if it cannot be queried.
std::optional<std::wstring> ProcessImagePath(unsigned long pid);

// Process creation time as a FILETIME packed into a uint64, or nullopt.
std::optional<unsigned long long> ProcessCreationTime(unsigned long pid);

// True if the pid refers to a live process.
bool ProcessAlive(unsigned long pid);

// OS gates.
bool Is64BitWindows();
bool IsWindows10OrGreater();

} // namespace cheburnet::probe
