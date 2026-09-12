#ifndef EXAMPLES_POLLUXDESK_ACTIVITY_ACTIVITY_DATA_H_
#define EXAMPLES_POLLUXDESK_ACTIVITY_ACTIVITY_DATA_H_

// PolluxOS "activity" readings: CPU, memory, disks, network and the process
// table.
//
// Everything here comes from the kernel's own interfaces -- sysctl, the
// process table (KERN_PROC_ALL), getfsstat() and getifaddrs() -- rather than
// from parsing the output of top/df/netstat.  That keeps the numbers exact,
// costs a few hundred microseconds per sample instead of several processes,
// and means the window keeps working when those tools are not installed.
//
// Nothing in this header depends on dui, so the whole data path can be built
// and exercised by a small program on its own (see the throwaway test that was
// used to check every reading before the window was written).

#include <string>
#include <vector>

namespace activity {

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------
struct CpuReading
{
    int    ncpu = 0;
    double busy = 0.0;                             // 0..100, all cores together
    double user = 0.0, sys = 0.0, idle = 100.0;    // percentages, sum to 100
    std::vector<double> coreBusy;                  // one 0..100 entry per core
    double load[3] = { 0.0, 0.0, 0.0 };
    double temperature = -1.0;                     // Celsius; < 0 when unknown
    long   uptime = 0;                             // seconds since boot
};

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
struct MemReading
{
    unsigned long long total = 0;      // bytes of physical memory
    unsigned long long used = 0;       // total - free, the way top counts it
    unsigned long long active = 0;
    unsigned long long inactive = 0;
    unsigned long long wired = 0;
    unsigned long long cached = 0;
    unsigned long long freeB = 0;
    unsigned long long swapTotal = 0;
    double usedPercent = 0.0;
};

// ---------------------------------------------------------------------------
// Processes
// ---------------------------------------------------------------------------
struct ProcessInfo
{
    int    pid = 0;
    std::string name;
    char   state = '?';                // 'R', 'S', 'Z', ...
    double cpu = 0.0;                  // percent of one core; may exceed 100
    unsigned long long rss = 0;        // resident bytes
    int    threads = 0;
    double cpuSeconds = 0.0;           // total CPU time charged so far
};

// ---------------------------------------------------------------------------
// Disks and network
// ---------------------------------------------------------------------------
struct DiskInfo
{
    std::string mount, device, type;
    unsigned long long total = 0, used = 0, avail = 0;
    double usedPercent = 0.0;
};

struct NetInfo
{
    std::string name;                  // "em0", "wlan0", ...
    bool   up = false;
    unsigned long long rxBytes = 0, txBytes = 0;   // since the interface came up
    double rxRate = 0.0, txRate = 0.0;             // bytes per second
    std::vector<std::string> addresses;
};

/** One sampler per window.  It remembers the previous counter values, because
 *  every rate here is a difference: the kernel reports totals, not speeds. */
class Sampler
{
public:
    /** @param out  CPU percentages, per-core load and the machine's own
     *              load average.  The first call reports the average since
     *              boot (there is nothing to subtract yet) and every call
     *              after it reports the change since the previous one. */
    void SampleCpu(CpuReading& out);

    void SampleMemory(MemReading& out);

    /** Every process, in process-table order (pid order).  CPU percent is the
     *  change in charged CPU time over the interval, so it is what the process
     *  is doing now rather than a five-minute average. */
    void SampleProcesses(std::vector<ProcessInfo>& out);

    /** Real filesystems only: devfs, procfs and friends are left out. */
    void SampleDisks(std::vector<DiskInfo>& out);

    /** Interfaces except the loopback, busiest first. */
    void SampleNetwork(std::vector<NetInfo>& out);

    /** The single-letter process state, spelled out. */
    static std::string StateText(char state);

private:
    struct CpuTick
    {
        unsigned long long user = 0, nice = 0, sys = 0, intr = 0, idle = 0;
        unsigned long long Total() const { return user + nice + sys + intr + idle; }
    };

    std::vector<CpuTick> m_prevCpu;
    bool m_haveCpu = false;

    // pid -> CPU seconds charged, and the wall clock they were read at.
    std::vector<std::pair<int, double>> m_prevProc;
    double m_prevProcTime = 0.0;
    bool   m_haveProc = false;

    struct NetCounters { unsigned long long rx = 0, tx = 0; };
    std::vector<std::pair<std::string, NetCounters>> m_prevNet;
    double m_prevNetTime = 0.0;
    bool   m_haveNet = false;
};

}  // namespace activity

#endif  // EXAMPLES_POLLUXDESK_ACTIVITY_ACTIVITY_DATA_H_
