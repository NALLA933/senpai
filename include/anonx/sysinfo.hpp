// AnonXMusic C++ port — Phase 6b (admin & menu commands)
// sysinfo.hpp — host metrics for /ping and /stats.
//
// The Python bot reads these through `psutil` (cpu_percent, virtual_memory,
// disk_usage) plus `platform`. There is no psutil for C++, so this is a small
// POSIX reader: /proc/stat for CPU, /proc/meminfo for RAM, statvfs for disk,
// uname for the platform string.
//
// Every getter is virtual for the same reason YouTube's subprocess calls are:
// a test needs deterministic numbers. `FakeSystemInfo` (test/fake_sysinfo)
// returns scripted values so the /ping and /stats cards can be asserted exactly.
//
// Nothing here throws: an unreadable /proc file yields 0 (or an empty string),
// which renders as "0" in the card rather than taking the bot down.

#ifndef ANONX_SYSINFO_HPP
#define ANONX_SYSINFO_HPP

#include <chrono>
#include <cstdint>
#include <string>

namespace anonx {

class SystemInfo {
public:
    SystemInfo();
    virtual ~SystemInfo() = default;

    // CPU utilisation over the interval since the PREVIOUS call, in percent
    // (psutil.cpu_percent(interval=None) semantics). The first call samples for
    // ~100 ms so it has something to compare against.
    virtual double cpuPercent();

    // RAM: used share of total, used megabytes, total gigabytes.
    virtual double       ramPercent();
    virtual std::int64_t ramUsedMb();
    virtual double       ramTotalGb();

    // Disk usage of the filesystem holding the working directory.
    virtual double diskPercent();
    virtual double diskUsedGb();
    virtual double diskTotalGb();

    virtual int         cores();
    virtual std::string platform();          // e.g. "Linux 6.1.0 x86_64"

    // Seconds since this object was constructed — i.e. process uptime, which is
    // what the Python bot reports (it timestamps `boot_time` at startup).
    virtual std::int64_t uptimeSeconds();

    // Version strings for the stats card. The Python template lists
    // Python/Pyrogram/PyTgCalls; this port substitutes its own three, and
    // AdminPlugins labels them accordingly.
    virtual std::string toolchainVersion();  // "C++17 (g++ 13.2.0)"
    virtual std::string telegramLibrary();   // "TDLib (JSON interface)"
    virtual std::string voiceLibrary();      // "NTgCalls"

    // "1d 2h 3m 4s", skipping leading zero units ("45s" for 45). Pure helper,
    // exposed for tests and reuse.
    static std::string formatDuration(std::int64_t seconds);

    // Round to one decimal place and render without a trailing ".0" artefact,
    // matching how the cards print percentages and gigabytes.
    static std::string round1(double value);

protected:
    // Total and idle jiffies from /proc/stat. Returns false if unreadable.
    static bool readCpuJiffies(std::uint64_t& total, std::uint64_t& idle);

private:
    std::chrono::steady_clock::time_point start_;
    std::uint64_t lastTotal_ = 0;
    std::uint64_t lastIdle_  = 0;
    bool          sampled_   = false;
};

}  // namespace anonx

#endif  // ANONX_SYSINFO_HPP
