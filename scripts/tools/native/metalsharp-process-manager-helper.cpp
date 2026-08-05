// metalsharp-process-manager-helper
//
// Standalone arm64 telemetry probe for the MetalSharp Process Manager overlay.
// Prints one JSON object to stdout and exits. The Electron main process runs
// this on a 1.5s poll; if it is missing/fails, the JS fallback takes over.
//
// Fields collected:
//   - processes: `/bin/ps -axo pid=,pcpu=,pmem=,comm=,command=`, filtered to the
//     Wine/Steam/MetalSharp session, with per-PID FPS attached from
//     /tmp/metalsharp-fps-<pid>/session-fps when present.
//   - cpu_percent / cores_used: getloadavg (system-wide), cores_total: sysctl.
//   - ram_used/total: sysctl hw.memsize + mach host_statistics64.
//   - gpu_percent: IOKit IOGraphicsAccelerator "Device Utilization %".
//   - cpu_temp_c: best-effort IOKit thermal sensor (null when unavailable).
//   - fps: most-recent session-fps file (null until the DXMT present-rate
//     writer exists — a follow-up; the overlay shows "WAIT"/"-- FPS" then).
//
// Build: scripts/tools/native/build-helper.sh
// Link: -framework IOKit -framework CoreFoundation

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <chrono>
#include <ctype.h>
#include <glob.h>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_init.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

struct PsRow {
  int pid;
  double pcpu;
  double pmem;
  std::string name;
  std::string command;
};

static std::string basename_of(const std::string &cmd) {
  size_t pos = std::string::npos;
  for (char sep : {'/', '\\'}) {
    size_t p = cmd.find_last_of(sep);
    if (p != std::string::npos && (pos == std::string::npos || p > pos)) pos = p;
  }
  return pos == std::string::npos ? cmd : cmd.substr(pos + 1);
}

static std::vector<PsRow> read_ps() {
  std::vector<PsRow> rows;
  // Drop `comm=` — under Wine it can be a Windows path containing spaces,
  // which defeats whitespace splitting. Derive name from command instead.
  FILE *pipe = popen("/bin/ps -axo pid=,pcpu=,pmem=,command=", "r");
  if (!pipe) return rows;
  char *line = nullptr;
  size_t cap = 0;
  while (getline(&line, &cap, pipe) != -1) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    std::istringstream iss(s);
    std::string pid_s, pcpu_s, pmem_s;
    if (!(iss >> pid_s >> pcpu_s >> pmem_s)) continue;
    PsRow r;
    r.pid = atoi(pid_s.c_str());
    r.pcpu = atof(pcpu_s.c_str());
    r.pmem = atof(pmem_s.c_str());
    std::string rest;
    std::getline(iss, rest);
    while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
    r.command = rest;
    r.name = basename_of(rest);
    if (r.pid > 0) rows.push_back(r);
  }
  free(line);
  pclose(pipe);
  return rows;
}

static bool session_interesting(const PsRow &r) {
  std::string hay = r.name + " " + r.command;
  for (auto &c : hay) c = tolower(c);
  // Isolation contract: bare `wine`/`wineserver` tokens are not enough — a
  // foreign Wine launcher (CrossOver, SakuraGiri, Whisky, GPTK) also runs
  // those names. A process is interesting only when it references the
  // MetalSharp home/prefix (runtime, prefix-steam, bottles, sharp-prefix) or
  // the metalsharp-wine wrapper, or is a Steam client process inside the MS
  // prefix (drive_c/... steam paths only make sense under an MS prefix).
  bool ms_owned = hay.find("metalsharp") != std::string::npos ||
                  hay.find(".metalsharp/") != std::string::npos ||
                  hay.find("prefix-steam") != std::string::npos ||
                  hay.find("sharp-prefix") != std::string::npos ||
                  hay.find("/bottles/") != std::string::npos;
  if (ms_owned) return true;
  bool steam_in_prefix = hay.find("drive_c/") != std::string::npos &&
                         hay.find("steam") != std::string::npos;
  return steam_in_prefix;
}

// FPS files: /tmp/metalsharp-fps-<pid>/session-fps contains a leading number.
// Returns true if a fresh FPS file exists for `pid` within `stale_s` seconds.
static bool read_pid_fps(int pid, double stale_s, double *out_fps, bool *out_fresh) {
  char dir[256];
  snprintf(dir, sizeof(dir), "/tmp/metalsharp-fps-%d", pid);
  fs::path f = fs::path(dir) / "session-fps";
  std::error_code ec;
  if (!fs::exists(f, ec)) return false;
  std::ifstream in(f);
  if (!in) return false;
  double v = 0;
  in >> v;
  if (in.fail()) return false;
  auto mtime = fs::last_write_time(f, ec);
  auto now = fs::file_time_type::clock::now();
  double age =
      std::chrono::duration<double>(now - mtime).count();
  if (out_fps) *out_fps = v;
  if (out_fresh) *out_fresh = age <= stale_s;
  return true;
}

// Top-level FPS: newest session-fps across all metalsharp-fps-* dirs.
static bool read_top_fps(double stale_s, double *out_fps, bool *out_fresh, std::string &out_source) {
  glob_t g;
  std::string newest_source;
  double newest_age = 1e18;
  double newest_fps = 0;
  bool found = false;
  if (glob("/tmp/metalsharp-fps-*/session-fps", 0, nullptr, &g) == 0) {
    for (size_t i = 0; i < g.gl_pathc; i++) {
      fs::path f(g.gl_pathv[i]);
      std::error_code ec;
      std::ifstream in(f);
      if (!in) continue;
      double v = 0;
      in >> v;
      if (in.fail()) continue;
      auto mtime = fs::last_write_time(f, ec);
      auto now = fs::file_time_type::clock::now();
      double age = std::chrono::duration<double>(now - mtime).count();
      if (age < newest_age) {
        newest_age = age;
        newest_fps = v;
        newest_source = std::string(g.gl_pathv[i]);
        found = true;
      }
    }
    globfree(&g);
  }
  if (!found) return false;
  if (out_fps) *out_fps = newest_fps;
  if (out_fresh) *out_fresh = newest_age <= stale_s;
  out_source = "session-fps file";
  return true;
}

static long sysctl_long(const char *key) {
  long val = 0;
  size_t len = sizeof(val);
  if (sysctlbyname(key, &val, &len, nullptr, 0) != 0) return 0;
  return val;
}

static std::string sysctl_str(const char *key) {
  char buf[256] = {0};
  size_t len = sizeof(buf) - 1;
  if (sysctlbyname(key, buf, &len, nullptr, 0) != 0) return "";
  return std::string(buf);
}

static double gpu_utilization_percent(std::string &out_label) {
  out_label = "system GPU telemetry unavailable";
  double pct = -1;
  // On Apple Silicon, AGXAccelerator (and its subclass of AGXAcceleratorG16G)
  // is the correct match. IOGraphicsAccelerator is the base class.
  const char *classes[] = {"AGXAccelerator", "IOGraphicsAccelerator"};
  for (const char *cls : classes) {
    CFMutableDictionaryRef match = IOServiceMatching(cls);
    if (!match) continue;
    io_iterator_t iter;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, match, &iter) != kIOReturnSuccess) continue;
    io_registry_entry_t entry;
    while ((entry = IOIteratorNext(iter)) != 0) {
      // "Device Utilization %" lives inside the nested PerformanceStatistics
      // dictionary, not as a top-level property.
      CFTypeRef perf = IORegistryEntryCreateCFProperty(entry, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
      if (perf && CFGetTypeID(perf) == CFDictionaryGetTypeID()) {
        CFTypeRef util = CFDictionaryGetValue((CFDictionaryRef)perf, CFSTR("Device Utilization %"));
        if (util && CFGetTypeID(util) == CFNumberGetTypeID()) {
          long long v = 0;
          if (CFNumberGetValue((CFNumberRef)util, kCFNumberLongLongType, &v)) pct = (double)v;
        }
      }
      if (perf) CFRelease(perf);
      if (pct >= 0) {
        out_label = "AGX Device Utilization % via ioreg PerformanceStatistics";
        IOObjectRelease(entry);
        break;
      }
      IOObjectRelease(entry);
    }
    IOObjectRelease(iter);
    if (pct >= 0) break;
  }
  return pct;
}

// CPU temperature on Apple Silicon is gated behind a root-only entitlement
// (AppleSMC / powermetrics / IOReport private SPI). On unified SoC (M-series),
// CPU and GPU share the same package — the GPU junction temp IS the die temp.
// We try the PerformanceStatistics dictionary first (which MAY contain
// temperature keys on some GPU generations), then fall back to honest null.
// When null, the overlay displays "SENSOR" as an honest label.
static bool cpu_temp_c(double *out, std::string &out_source) {
  const char *classes[] = {"AGXAccelerator", "IOGraphicsAccelerator"};
  for (const char *cls : classes) {
    CFMutableDictionaryRef match = IOServiceMatching(cls);
    if (!match) continue;
    io_iterator_t iter;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, match, &iter) != kIOReturnSuccess) continue;
    io_registry_entry_t entry;
    while ((entry = IOIteratorNext(iter)) != 0) {
      CFTypeRef perf = IORegistryEntryCreateCFProperty(entry, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
      if (perf && CFGetTypeID(perf) == CFDictionaryGetTypeID()) {
        const char *temp_keys[] = {"hottests junc temp", "GPU Temperature", "hottest junc", "junction temperature"};
        for (const char *key : temp_keys) {
          CFStringRef cf_key = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
          CFTypeRef val = CFDictionaryGetValue((CFDictionaryRef)perf, cf_key);
          CFRelease(cf_key);
          if (val) {
            double v = 0;
            if (CFGetTypeID(val) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)val, kCFNumberDoubleType, &v);
            if (v > 0 && v < 200) {
              *out = v;
              char buf[256];
              snprintf(buf, sizeof(buf), "SoC temp via GPU PerformanceStatistics \"%s\"", key);
              out_source = buf;
              CFRelease(perf);
              IOObjectRelease(entry);
              IOObjectRelease(iter);
              return true;
            }
          }
        }
      }
      if (perf) CFRelease(perf);
      IOObjectRelease(entry);
    }
    IOObjectRelease(iter);
  }
  (void)out;
  (void)out_source;
  return false;
}

static void ram_bytes(uint64_t *used, uint64_t *total) {
  *total = 0;
  *used = 0;
  int64_t memsize = 0;
  size_t len = sizeof(memsize);
  if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0) *total = (uint64_t)memsize;

  vm_size_t page_size = 0;
  host_page_size(mach_host_self(), &page_size);
  vm_statistics64_data_t vm;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) == KERN_SUCCESS) {
    uint64_t free_bytes = (uint64_t)vm.free_count * page_size;
    uint64_t inactive_bytes = (uint64_t)vm.inactive_count * page_size;
    uint64_t used_bytes = *total > (free_bytes + inactive_bytes) ? *total - (free_bytes + inactive_bytes) : 0;
    *used = used_bytes;
  }
}

int main() {
  const double FPS_STALE_S = 3.0;

  int cores_total = (int)sysctl_long("hw.logicalcpu");
  if (cores_total < 1) cores_total = 1;
  std::string chip = sysctl_str("machdep.cpu.brand_string");
  if (chip.empty()) chip = sysctl_str("hw.optional.arm64");

  double load[3] = {0, 0, 0};
  getloadavg(load, 3);
  double cpu_percent = std::min(100.0, std::round((load[0] / cores_total) * 1000.0) / 10.0);
  double cores_used = std::round(load[0] * 10.0) / 10.0;

  uint64_t ram_used = 0, ram_total = 0;
  ram_bytes(&ram_used, &ram_total);

  std::string gpu_label;
  double gpu = gpu_utilization_percent(gpu_label);
  // Read renderer utilization and GPU memory from PerformanceStatistics
  // (separate probe — avoids modifying the existing GPU util function)
  double renderer_pct = -1;
  uint64_t gpu_mem_used = 0;
  io_iterator_t rit;
  if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("AGXAccelerator"), &rit) == kIOReturnSuccess) {
    io_registry_entry_t re;
    while ((re = IOIteratorNext(rit)) != 0) {
      CFTypeRef perf = IORegistryEntryCreateCFProperty(re, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
      if (perf && CFGetTypeID(perf) == CFDictionaryGetTypeID()) {
        CFTypeRef rp = CFDictionaryGetValue((CFDictionaryRef)perf, CFSTR("Renderer Utilization %%"));
        if (rp && CFGetTypeID(rp) == CFNumberGetTypeID()) { long long rv = 0; CFNumberGetValue((CFNumberRef)rp, kCFNumberLongLongType, &rv); renderer_pct = (double)rv; }
        CFTypeRef mu = CFDictionaryGetValue((CFDictionaryRef)perf, CFSTR("In use system memory"));
        if (mu && CFGetTypeID(mu) == CFNumberGetTypeID()) { long long mv = 0; CFNumberGetValue((CFNumberRef)mu, kCFNumberLongLongType, &mv); gpu_mem_used = (uint64_t)mv; }
      }
      if (perf) CFRelease(perf);
      IOObjectRelease(re);
    }
    IOObjectRelease(rit);
  }

  double cpu_temp = 0;
  std::string cpu_temp_source;
  bool has_temp = cpu_temp_c(&cpu_temp, cpu_temp_source);

  double top_fps = 0;
  bool top_fresh = false;
  std::string fps_source;
  bool has_fps = read_top_fps(FPS_STALE_S, &top_fps, &top_fresh, fps_source);

  auto rows = read_ps();
  std::vector<PsRow> session;
  for (auto &r : rows)
    if (session_interesting(r)) session.push_back(r);

  // JSON output
  std::ostringstream o;
  o << "{";
  o << "\"ok\":true,";
  o << "\"source\":\"metalsharp-process-helper-cpp\",";
  o << "\"timestamp\":" << (long long)time(nullptr) << ",";
  o << "\"cpu_percent\":" << cpu_percent << ",";
  o << "\"cores_used\":" << cores_used << ",";
  o << "\"cores_total\":" << cores_total << ",";
  o << "\"ram_used_bytes\":" << ram_used << ",";
  o << "\"ram_total_bytes\":" << ram_total << ",";
  o << "\"chip\":\"" << json_escape(chip) << "\",";
  if (has_temp) {
    o << "\"cpu_temp_c\":" << std::round(cpu_temp) << ",";
    o << "\"cpu_temp_source\":\"" << json_escape(cpu_temp_source) << "\",";
  } else {
    o << "\"cpu_temp_c\":null,";
  }
  if (renderer_pct >= 0) o << "\"renderer_percent\":" << renderer_pct << ",";
  if (gpu_mem_used > 0) o << "\"gpu_mem_used_bytes\":" << gpu_mem_used << ",";
  if (gpu >= 0) {
    o << "\"gpu_percent\":" << std::round(gpu) << ",";
  } else {
    o << "\"gpu_percent\":null,";
  }
  o << "\"gpu_label\":\"" << json_escape(gpu_label) << "\",";
  if (has_fps) {
    o << "\"fps\":" << top_fps << ",";
    o << "\"fps_fresh\":" << (top_fresh ? "true" : "false") << ",";
    o << "\"fps_source\":\"" << json_escape(fps_source) << "\",";
  } else {
    o << "\"fps\":null,";
    o << "\"fps_source\":\"waiting for non-Steam Wine present telemetry\",";
  }
  o << "\"processes\":[";
  for (size_t i = 0; i < session.size(); i++) {
    if (i) o << ",";
    const PsRow &r = session[i];
    double pfps = 0;
    bool pfresh = false;
    bool has_pfps = read_pid_fps(r.pid, FPS_STALE_S, &pfps, &pfresh);
    o << "{";
    o << "\"pid\":" << r.pid << ",";
    o << "\"name\":\"" << json_escape(r.name) << "\",";
    o << "\"command\":\"" << json_escape(r.command) << "\",";
    o << "\"cpu_percent\":" << r.pcpu << ",";
    o << "\"mem_percent\":" << r.pmem << ",";
    if (has_pfps) {
      o << "\"fps\":" << pfps << ",";
      o << "\"fps_fresh\":" << (pfresh ? "true" : "false");
    } else {
      o << "\"fps\":null,";
      o << "\"fps_fresh\":false";
    }
    o << "}";
  }
  o << "]}";
  printf("%s\n", o.str().c_str());
  return 0;
}
