// Keeps the busiest threads of the process on dedicated big cores.
//
// Six threads carry the frame (guest render thread, recorder, command worker,
// and three lighter ones) and the scheduler moves them freely: the guest
// render thread was seen on a little core while the prime core idled, and the
// recorder spent ~30% of each frame off-CPU. Every two seconds this ranks the
// process's threads by CPU time over the last interval, gives the top ones a
// big core each (fastest first) and confines every other thread to the
// remaining cores. Threads are re-ranked continuously, so roles that move
// between threads (loading, cutscenes) follow.
//
// Audio threads (ours at nice -16, AAudio's realtime callback) are never
// confined: on the leftover cores they compete with streaming and loading,
// and the little cores alone cannot decode XMA through a load spike.
#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include <android/log.h>

#include <rex/cvar.h>

REXCVAR_DEFINE_UINT32(gta4_thread_pinning, 0, "GTA IV/Performance",
                      "Give the N busiest threads a big core each and keep the others off them "
                      "(0 = leave scheduling to Android)")
    .range(0, 4);

namespace {

struct ThreadStat {
  uint64_t ticks = 0;
  bool audio = false;  // high priority or realtime: left on every core
};

ThreadStat ReadThreadStat(pid_t tid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
  FILE* file = std::fopen(path, "r");
  if (!file) return {};
  char buffer[512];
  const size_t length = std::fread(buffer, 1, sizeof(buffer) - 1, file);
  std::fclose(file);
  buffer[length] = 0;
  // Fields after the parenthesized name: state is field 3, utime 14, stime 15.
  const char* p = std::strrchr(buffer, ')');
  if (!p) return {};
  // Field 19 is nice, 41 the scheduling policy (1 FIFO, 2 RR).
  unsigned long long utime = 0, stime = 0;
  long nice = 0, policy = 0;
  int field = 2;
  for (const char* c = p + 1; *c && field < 41; ++c) {
    if (*c == ' ') {
      ++field;
      if (field == 14) utime = std::strtoull(c + 1, nullptr, 10);
      if (field == 15) stime = std::strtoull(c + 1, nullptr, 10);
      if (field == 19) nice = std::strtol(c + 1, nullptr, 10);
      if (field == 41) policy = std::strtol(c + 1, nullptr, 10);
    }
  }
  return {utime + stime, nice <= -16 ||policy == 1 || policy == 2};
}

long ReadSysfsLong(const char* format, int cpu) {
  char path[96];
  std::snprintf(path, sizeof(path), format, cpu);
  long value = 0;
  if (FILE* file = std::fopen(path, "r")) {
    if (std::fscanf(file, "%ld", &value) != 1) value = 0;
    std::fclose(file);
  }
  return value;
}

// Fastest core first. The kernel's cpu_capacity (normalized per-core performance)
// ranks first: on mid-range SoCs such as the Helio G200 (2x A76 at 2.2 GHz,
// 6x A55 at 2.0 GHz) the clock difference alone is too small to tell big from
// little reliably. Max frequency breaks ties, then the lower core number.
std::vector<int> CoresByMaxFrequency() {
  struct Core {
    long capacity;
    long frequency;
    int cpu;
  };
  std::vector<Core> cores;
  const long count = sysconf(_SC_NPROCESSORS_CONF);
  for (int cpu = 0; cpu < count && cpu < CPU_SETSIZE; ++cpu) {
    cores.push_back(
        {ReadSysfsLong("/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu),
         ReadSysfsLong("/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu), cpu});
  }
  std::stable_sort(cores.begin(), cores.end(), [](const Core& a, const Core& b) {
    if (a.capacity != b.capacity) return a.capacity > b.capacity;
    return a.frequency > b.frequency;
  });
  std::vector<int> result;
  for (const Core& core : cores) result.push_back(core.cpu);
  return result;
}

void SetAffinity(pid_t tid, const cpu_set_t& set) {
  syscall(__NR_sched_setaffinity, tid, sizeof(set), &set);
}

void PinningLoop() {
  const std::vector<int> cores = CoresByMaxFrequency();
  if (cores.size() < 6) return;
  std::unordered_map<pid_t, uint64_t> previous;
  uint32_t applied = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const uint32_t pinned = std::min<uint32_t>(REXCVAR_GET(gta4_thread_pinning), 4);
    std::vector<std::pair<uint64_t, pid_t>> usage;
    std::vector<pid_t> audio;
    std::unordered_map<pid_t, uint64_t> current;
    if (DIR* directory = opendir("/proc/self/task")) {
      while (dirent* entry = readdir(directory)) {
        const pid_t tid = pid_t(std::atoi(entry->d_name));
        if (tid <= 0) continue;
        const ThreadStat stat = ReadThreadStat(tid);
        const uint64_t ticks = stat.ticks;
        current[tid] = ticks;
        if (stat.audio) {
          audio.push_back(tid);
          continue;
        }
        const auto old = previous.find(tid);
        usage.emplace_back(old != previous.end() && ticks >= old->second ? ticks - old->second : 0,
                           tid);
      }
      closedir(directory);
    }
    previous.swap(current);
    if (!pinned && !applied) continue;
    std::sort(usage.rbegin(), usage.rend());
    cpu_set_t rest;
    CPU_ZERO(&rest);
    for (size_t i = pinned; i < cores.size(); ++i) CPU_SET(cores[i], &rest);
    for (size_t i = 0; i < usage.size(); ++i) {
      cpu_set_t set;
      CPU_ZERO(&set);
      if (!pinned) {
        for (int cpu : cores) CPU_SET(cpu, &set);
      } else if (i < pinned) {
        CPU_SET(cores[i], &set);
      } else {
        set = rest;
      }
      SetAffinity(usage[i].second, set);
    }
    cpu_set_t all;
    CPU_ZERO(&all);
    for (int cpu : cores) CPU_SET(cpu, &all);
    for (pid_t tid : audio) SetAffinity(tid, all);
    if (pinned != applied) {
      __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "thread-pinning: %u threads on cores %d,%d,%d",
                          pinned, cores[0], cores[1], cores[2]);
    }
    applied = pinned;
  }
}

const bool g_pinning_started = [] {
  std::thread(PinningLoop).detach();
  return true;
}();

}  // namespace
