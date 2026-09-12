#include "ActivityData.h"

#include <sys/param.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace activity {
namespace {

// CPUSTATES from <sys/resource.h>: user, nice, sys, intr, idle.  Named here
// because kern.cp_times is a flat long[ncpu][CPUSTATES].
const int kCpuStates = 5;

unsigned long long SysctlU64(const char* name, unsigned long long fallback = 0)
{
    unsigned long long value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) {
        return fallback;
    }
    return value;
}

unsigned int SysctlUInt(const char* name, unsigned int fallback = 0)
{
    unsigned int value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) {
        return fallback;
    }
    return value;
}

double MonotonicSeconds()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

double Seconds(const struct timeval& tv)
{
    return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
}

// The thermal sysctls are not one thing on every machine.  coretemp's
// dev.cpu.N.temperature is an int holding tenths of a kelvin -- sysctl(8)
// prints it as "57.1C" thanks to its "IK" format hint, but the raw value is
// 3301 -- while hw.acpi.thermal.tz0.temperature is a string.  Both are tried,
// and an unreadable one leaves the caller's value alone.
bool ReadTemperature(double& out)
{
    const char* intNames[] = { "dev.cpu.0.temperature" };
    for (const char* name : intNames) {
        int tenthsKelvin = 0;
        size_t size = sizeof(tenthsKelvin);
        if (sysctlbyname(name, &tenthsKelvin, &size, nullptr, 0) != 0) {
            continue;
        }
        const double celsius = static_cast<double>(tenthsKelvin) / 10.0 - 273.15;
        if (celsius > -50.0 && celsius < 200.0) {
            out = celsius;
            return true;
        }
    }

    char buf[64];
    const char* textNames[] = { "hw.acpi.thermal.tz0.temperature",
                                "dev.cpu.0.temperature" };
    for (const char* name : textNames) {
        size_t size = sizeof(buf);
        if (sysctlbyname(name, buf, &size, nullptr, 0) != 0) {
            continue;
        }
        const double value = std::atof(buf);
        if (value > -50.0 && value < 200.0) {
            out = value;
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------
void Sampler::SampleCpu(CpuReading& out)
{
    out = CpuReading();

    size_t len = 0;
    if (sysctlbyname("kern.cp_times", nullptr, &len, nullptr, 0) != 0 ||
        len < sizeof(long) * kCpuStates) {
        return;
    }
    std::vector<long> raw(len / sizeof(long));
    if (sysctlbyname("kern.cp_times", raw.data(), &len, nullptr, 0) != 0) {
        return;
    }
    const size_t values = len / sizeof(long);
    const int ncpu = static_cast<int>(values / kCpuStates);
    if (ncpu <= 0) {
        return;
    }
    out.ncpu = ncpu;

    std::vector<CpuTick> now(static_cast<size_t>(ncpu));
    for (int cpu = 0; cpu < ncpu; ++cpu) {
        const long* t = &raw[static_cast<size_t>(cpu) * kCpuStates];
        CpuTick& tick = now[static_cast<size_t>(cpu)];
        tick.user = static_cast<unsigned long long>(t[0]);
        tick.nice = static_cast<unsigned long long>(t[1]);
        tick.sys  = static_cast<unsigned long long>(t[2]);
        tick.intr = static_cast<unsigned long long>(t[3]);
        tick.idle = static_cast<unsigned long long>(t[4]);
    }

    // First reading has nothing to subtract, so it reports the average since
    // boot -- which is exactly what the cumulative counters already are.
    const bool havePrevious = m_haveCpu && m_prevCpu.size() == now.size();
    CpuTick sum;
    out.coreBusy.assign(static_cast<size_t>(ncpu), 0.0);

    for (int cpu = 0; cpu < ncpu; ++cpu) {
        CpuTick delta;
        const CpuTick& cur = now[static_cast<size_t>(cpu)];
        if (havePrevious) {
            const CpuTick& prev = m_prevCpu[static_cast<size_t>(cpu)];
            delta.user = cur.user - prev.user;
            delta.nice = cur.nice - prev.nice;
            delta.sys  = cur.sys  - prev.sys;
            delta.intr = cur.intr - prev.intr;
            delta.idle = cur.idle - prev.idle;
        } else {
            delta = cur;
        }
        const unsigned long long total = delta.Total();
        if (total > 0) {
            out.coreBusy[static_cast<size_t>(cpu)] =
                100.0 * static_cast<double>(total - delta.idle) /
                static_cast<double>(total);
        }
        sum.user += delta.user;
        sum.nice += delta.nice;
        sum.sys  += delta.sys;
        sum.intr += delta.intr;
        sum.idle += delta.idle;
    }

    const unsigned long long total = sum.Total();
    if (total > 0) {
        const double scale = 100.0 / static_cast<double>(total);
        out.user = static_cast<double>(sum.user + sum.nice) * scale;
        out.sys  = static_cast<double>(sum.sys + sum.intr) * scale;
        out.idle = static_cast<double>(sum.idle) * scale;
        out.busy = 100.0 - out.idle;
    }

    m_prevCpu = std::move(now);
    m_haveCpu = true;

    getloadavg(out.load, 3);
    ReadTemperature(out.temperature);

    struct timeval boot;
    size_t bootLen = sizeof(boot);
    if (sysctlbyname("kern.boottime", &boot, &bootLen, nullptr, 0) == 0) {
        const long nowSeconds = static_cast<long>(std::time(nullptr));
        out.uptime = nowSeconds - static_cast<long>(boot.tv_sec);
        if (out.uptime < 0) {
            out.uptime = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
void Sampler::SampleMemory(MemReading& out)
{
    out = MemReading();

    const unsigned long long pageSize = SysctlUInt("vm.stats.vm.v_page_size", 4096);
    const unsigned long long pages = SysctlU64("vm.stats.vm.v_page_count");
    const unsigned long long freePages = SysctlU64("vm.stats.vm.v_free_count");

    if (pages > 0) {
        out.total = pages * pageSize;
        out.freeB = freePages * pageSize;
    } else {
        // No VM stats (an unusual kernel): fall back to what the firmware says.
        out.total = SysctlU64("hw.physmem");
        out.freeB = 0;
    }
    out.active   = SysctlU64("vm.stats.vm.v_active_count") * pageSize;
    out.inactive = SysctlU64("vm.stats.vm.v_inactive_count") * pageSize;
    out.wired    = SysctlU64("vm.stats.vm.v_wire_count") * pageSize;
    out.cached   = SysctlU64("vm.stats.vm.v_cache_count") * pageSize;
    out.swapTotal = SysctlU64("vm.swap_total");

    out.used = out.total > out.freeB ? out.total - out.freeB : 0;
    if (out.total > 0) {
        out.usedPercent = 100.0 * static_cast<double>(out.used) /
                          static_cast<double>(out.total);
    }
}

// ---------------------------------------------------------------------------
// Processes
// ---------------------------------------------------------------------------
void Sampler::SampleProcesses(std::vector<ProcessInfo>& out)
{
    out.clear();

    // Three elements, not four: the fourth element belongs to the KERN_PROC_PID
    // style lookups and makes this one fail with ENOTDIR.  KERN_PROC_PROC
    // rather than KERN_PROC_ALL because the latter returns one row per thread,
    // which turns a process list into a list of near-duplicates.
    int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC };
    size_t len = 0;
    if (sysctl(mib, 3, nullptr, &len, nullptr, 0) != 0 || len == 0) {
        return;
    }
    // The table can grow between the sizing call and the reading call, so ask
    // for room for a few more and take whatever comes back.
    std::vector<struct kinfo_proc> table(len / sizeof(struct kinfo_proc) + 16);
    len = table.size() * sizeof(struct kinfo_proc);
    if (sysctl(mib, 3, table.data(), &len, nullptr, 0) != 0) {
        return;
    }
    const size_t count = len / sizeof(struct kinfo_proc);
    const unsigned long long pageSize = SysctlUInt("vm.stats.vm.v_page_size", 4096);

    const double now = MonotonicSeconds();
    const double elapsed = (m_haveProc && now > m_prevProcTime)
                               ? now - m_prevProcTime : 0.0;

    std::vector<std::pair<int, double>> current;
    current.reserve(count);

    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const struct kinfo_proc& kp = table[i];
        if (kp.ki_pid == 0) {
            continue;   // the kernel's own idle/swapper entries
        }
        // The per-CPU idle threads are charged for the time their CPU spends
        // doing nothing, so a difference-based reading reports them as the
        // busiest thing on the machine by a factor of the core count.  ps and
        // top hide this behind their decayed percentage; a list of what is
        // running should simply not contain them.
        if ((kp.ki_flag & P_SYSTEM) != 0 && std::strcmp(kp.ki_comm, "idle") == 0) {
            continue;
        }
        ProcessInfo info;
        info.pid = kp.ki_pid;
        info.name = kp.ki_comm;
        info.state = kp.ki_stat;
        info.threads = kp.ki_numthreads;
        info.rss = static_cast<unsigned long long>(kp.ki_rssize) * pageSize;
        const double cpuSeconds = Seconds(kp.ki_rusage.ru_utime) +
                                  Seconds(kp.ki_rusage.ru_stime);
        info.cpuSeconds = cpuSeconds;

        // The change in charged CPU time over the interval: this is what the
        // process did in the last second, not a decaying five-minute average.
        // The kernel's own estimate is used on the very first sample, when
        // there is nothing to subtract.
        double percent = 0.0;
        bool fromDelta = false;
        for (const auto& entry : m_prevProc) {
            if (entry.first == info.pid && elapsed > 0.05 &&
                cpuSeconds >= entry.second) {
                percent = 100.0 * (cpuSeconds - entry.second) / elapsed;
                fromDelta = true;
                break;
            }
        }
        if (!fromDelta) {
            percent = 100.0 * static_cast<double>(kp.ki_pctcpu) / FSCALE;
        }
        info.cpu = percent;
        current.emplace_back(info.pid, cpuSeconds);
        out.push_back(std::move(info));
    }

    m_prevProc = std::move(current);
    m_prevProcTime = now;
    m_haveProc = true;
}

std::string Sampler::StateText(char state)
{
    switch (state) {
    case 1: return "空闲";     // SIDL
    case 2: return "运行";     // SRUN
    case 3: return "睡眠";     // SSLEEP
    case 4: return "停止";     // SSTOP
    case 5: return "僵尸";     // SZOMB
    case 6: return "等待";     // SWAIT
    case 7: return "锁定";     // SLOCK
    default: return "—";
    }
}

// ---------------------------------------------------------------------------
// Disks
// ---------------------------------------------------------------------------
void Sampler::SampleDisks(std::vector<DiskInfo>& out)
{
    out.clear();

    int count = getfsstat(nullptr, 0, MNT_NOWAIT);
    if (count <= 0) {
        return;
    }
    std::vector<struct statfs> volumes(static_cast<size_t>(count));
    count = getfsstat(volumes.data(), static_cast<long>(volumes.size() * sizeof(struct statfs)),
                      MNT_NOWAIT);
    if (count <= 0) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        const struct statfs& fs = volumes[static_cast<size_t>(i)];
        const std::string type = fs.f_fstypename;
        // Pseudo filesystems report sizes that mean nothing to a person
        // looking at how full their disk is.
        if (type == "devfs" || type == "procfs" || type == "linprocfs" ||
            type == "linsysfs" || type == "fdescfs" || type == "autofs") {
            continue;
        }
        DiskInfo info;
        info.mount = fs.f_mntonname;
        info.device = fs.f_mntfromname;
        info.type = type;
        const unsigned long long blockSize = fs.f_bsize;
        info.total = static_cast<unsigned long long>(fs.f_blocks) * blockSize;
        info.avail = static_cast<unsigned long long>(fs.f_bavail) * blockSize;
        info.used  = static_cast<unsigned long long>(fs.f_blocks - fs.f_bfree) * blockSize;
        if (info.total > 0) {
            info.usedPercent = 100.0 * static_cast<double>(info.used) /
                               static_cast<double>(info.total);
        }
        out.push_back(std::move(info));
    }

    std::sort(out.begin(), out.end(), [](const DiskInfo& a, const DiskInfo& b) {
        return a.mount < b.mount;
    });
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------
void Sampler::SampleNetwork(std::vector<NetInfo>& out)
{
    out.clear();

    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) {
        return;
    }

    for (struct ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_name == nullptr) {
            continue;
        }
        const std::string name = ifa->ifa_name;
        if (name == "lo0") {
            continue;   // loopback traffic is not what anyone opens this for
        }
        NetInfo* info = nullptr;
        for (NetInfo& existing : out) {
            if (existing.name == name) {
                info = &existing;
                break;
            }
        }
        if (info == nullptr) {
            out.emplace_back();
            info = &out.back();
            info->name = name;
            info->up = (ifa->ifa_flags & IFF_UP) != 0 &&
                       (ifa->ifa_flags & IFF_RUNNING) != 0;
        }

        if (ifa->ifa_addr->sa_family == AF_LINK) {
            const struct if_data* data =
                static_cast<const struct if_data*>(ifa->ifa_data);
            if (data != nullptr) {
                info->rxBytes = data->ifi_ibytes;
                info->txBytes = data->ifi_obytes;
            }
        } else if (ifa->ifa_addr->sa_family == AF_INET) {
            char text[INET_ADDRSTRLEN] = { 0 };
            const struct sockaddr_in* sin =
                reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_addr);
            if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) != nullptr) {
                info->addresses.push_back(text);
            }
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            char text[INET6_ADDRSTRLEN] = { 0 };
            const struct sockaddr_in6* sin6 =
                reinterpret_cast<const struct sockaddr_in6*>(ifa->ifa_addr);
            if (inet_ntop(AF_INET6, &sin6->sin6_addr, text, sizeof(text)) != nullptr) {
                const std::string address = text;
                // Link-local addresses are noise on a status panel.
                if (address.compare(0, 4, "fe80") != 0) {
                    info->addresses.push_back(address);
                }
            }
        }
    }
    freeifaddrs(ifap);

    // Rates are differences, so they need the previous totals and how long ago
    // they were taken.
    const double now = MonotonicSeconds();
    const double elapsed = (m_haveNet && now > m_prevNetTime) ? now - m_prevNetTime : 0.0;
    std::vector<std::pair<std::string, NetCounters>> current;
    current.reserve(out.size());
    for (NetInfo& info : out) {
        if (elapsed > 0.05) {
            for (const auto& previous : m_prevNet) {
                if (previous.first != info.name) {
                    continue;
                }
                if (info.rxBytes >= previous.second.rx) {
                    info.rxRate = static_cast<double>(info.rxBytes - previous.second.rx) / elapsed;
                }
                if (info.txBytes >= previous.second.tx) {
                    info.txRate = static_cast<double>(info.txBytes - previous.second.tx) / elapsed;
                }
                break;
            }
        }
        current.emplace_back(info.name, NetCounters{ info.rxBytes, info.txBytes });
    }
    m_prevNet = std::move(current);
    m_prevNetTime = now;
    m_haveNet = true;

    std::sort(out.begin(), out.end(), [](const NetInfo& a, const NetInfo& b) {
        if (a.up != b.up) {
            return a.up;
        }
        const double aTraffic = a.rxRate + a.txRate;
        const double bTraffic = b.rxRate + b.txRate;
        if (aTraffic != bTraffic) {
            return aTraffic > bTraffic;
        }
        return a.name < b.name;
    });
}

}  // namespace activity
