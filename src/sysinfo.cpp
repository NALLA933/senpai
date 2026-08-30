// AnonXMusic C++ port — Phase 6b (admin & menu commands)
// sysinfo.cpp — POSIX implementation of the host metrics (see sysinfo.hpp).

#include "anonx/sysinfo.hpp"

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace anonx {
namespace {

// One value from /proc/meminfo, in kibibytes (0 when the key is absent).
std::uint64_t memInfoKb(const char* key) {
    std::ifstream in("/proc/meminfo");
    if (!in)
        return 0;
    const std::size_t keyLen = std::strlen(key);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, keyLen, key) != 0 || line.size() <= keyLen ||
            line[keyLen] != ':')
            continue;
        std::istringstream ls(line.substr(keyLen + 1));
        std::uint64_t kb = 0;
        ls >> kb;
        return kb;
    }
    return 0;
}

constexpr double kKbPerGb = 1024.0 * 1024.0;   // KiB in a GiB

// Used = total - available (psutil's "used" for virtual_memory percent).
void memUsage(std::uint64_t& totalKb, std::uint64_t& usedKb) {
    totalKb = memInfoKb("MemTotal");
    const std::uint64_t availKb = memInfoKb("MemAvailable");
    usedKb = totalKb > availKb ? totalKb - availKb : 0;
}

bool diskUsage(double& usedGb, double& totalGb) {
    struct statvfs vfs;
    if (statvfs(".", &vfs) != 0)
        return false;
    const double block = static_cast<double>(vfs.f_frsize);
    const double total = block * static_cast<double>(vfs.f_blocks);
    const double free_ = block * static_cast<double>(vfs.f_bavail);
    const double gb = 1024.0 * 1024.0 * 1024.0;
    totalGb = total / gb;
    usedGb  = (total - free_) / gb;
    return total > 0.0;
}

}  // namespace

SystemInfo::SystemInfo() : start_(std::chrono::steady_clock::now()) {}

bool SystemInfo::readCpuJiffies(std::uint64_t& total, std::uint64_t& idle) {
    std::ifstream in("/proc/stat");
    if (!in)
        return false;
    std::string cpu;
    in >> cpu;
    if (cpu != "cpu")
        return false;
    // user nice system idle iowait irq softirq steal guest guest_nice
    std::uint64_t values[10] = {0};
    total = 0;
    for (std::size_t i = 0; i < 10 && (in >> values[i]); ++i)
        total += values[i];
    idle = values[3] + values[4];   // idle + iowait
    return total > 0;
}

double SystemInfo::cpuPercent() {
    std::uint64_t total = 0, idle = 0;
    if (!readCpuJiffies(total, idle))
        return 0.0;

    if (!sampled_) {
        // Nothing to diff against yet — take a short sample, like psutil's
        // cpu_percent(interval=0.1) on its first call.
        lastTotal_ = total;
        lastIdle_  = idle;
        sampled_   = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!readCpuJiffies(total, idle))
            return 0.0;
    }

    const double dTotal = static_cast<double>(total) - static_cast<double>(lastTotal_);
    const double dIdle  = static_cast<double>(idle)  - static_cast<double>(lastIdle_);
    lastTotal_ = total;
    lastIdle_  = idle;
    if (dTotal <= 0.0)
        return 0.0;
    const double busy = (dTotal - dIdle) / dTotal * 100.0;
    return busy < 0.0 ? 0.0 : (busy > 100.0 ? 100.0 : busy);
}

double SystemInfo::ramPercent() {
    std::uint64_t totalKb = 0, usedKb = 0;
    memUsage(totalKb, usedKb);
    if (totalKb == 0)
        return 0.0;
    return static_cast<double>(usedKb) / static_cast<double>(totalKb) * 100.0;
}

std::int64_t SystemInfo::ramUsedMb() {
    std::uint64_t totalKb = 0, usedKb = 0;
    memUsage(totalKb, usedKb);
    return static_cast<std::int64_t>(usedKb / 1024);
}

double SystemInfo::ramTotalGb() {
    return static_cast<double>(memInfoKb("MemTotal")) / kKbPerGb;
}

double SystemInfo::diskPercent() {
    double usedGb = 0.0, totalGb = 0.0;
    if (!diskUsage(usedGb, totalGb) || totalGb <= 0.0)
        return 0.0;
    return usedGb / totalGb * 100.0;
}

double SystemInfo::diskUsedGb() {
    double usedGb = 0.0, totalGb = 0.0;
    diskUsage(usedGb, totalGb);
    return usedGb;
}

double SystemInfo::diskTotalGb() {
    double usedGb = 0.0, totalGb = 0.0;
    diskUsage(usedGb, totalGb);
    return totalGb;
}

int SystemInfo::cores() {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 1;
}

std::string SystemInfo::platform() {
    struct utsname u;
    if (uname(&u) != 0)
        return "unknown";
    return std::string(u.sysname) + " " + u.release + " " + u.machine;
}

std::int64_t SystemInfo::uptimeSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(steady_clock::now() - start_).count();
}

std::string SystemInfo::toolchainVersion() {
#if defined(__clang__)
    return "C++17 (clang " __clang_version__ ")";
#elif defined(__GNUC__)
    return "C++17 (g++ " __VERSION__ ")";
#else
    return "C++17";
#endif
}

std::string SystemInfo::telegramLibrary() { return "TDLib (JSON interface)"; }

std::string SystemInfo::voiceLibrary() { return "NTgCalls"; }

std::string SystemInfo::formatDuration(std::int64_t seconds) {
    if (seconds < 0)
        seconds = 0;
    const std::int64_t d = seconds / 86400;
    const std::int64_t h = (seconds % 86400) / 3600;
    const std::int64_t m = (seconds % 3600) / 60;
    const std::int64_t s = seconds % 60;

    std::string out;
    if (d > 0) out += std::to_string(d) + "d ";
    if (d > 0 || h > 0) out += std::to_string(h) + "h ";
    if (d > 0 || h > 0 || m > 0) out += std::to_string(m) + "m ";
    out += std::to_string(s) + "s";
    return out;
}

std::string SystemInfo::round1(double value) {
    if (!std::isfinite(value))
        value = 0.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", value);
    return buf;
}

}  // namespace anonx
