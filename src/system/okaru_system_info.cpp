#include "system/okaru_system_info.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace okaru::sysinfo {

  namespace {

    std::string readFile(const char* path) {
      std::ifstream in(path);
      std::stringstream buffer;
      buffer << in.rdbuf();
      return buffer.str();
    }

    std::string trim(std::string s) {
      while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\t')) {
        s.pop_back();
      }
      while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(0, 1);
      }
      return s;
    }

    // \040 y compañía en /proc/mounts.
    std::string unescapeMount(std::string_view s) {
      std::string out;
      for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {
          out += static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
          i += 3;
        } else {
          out += s[i];
        }
      }
      return out;
    }

  } // namespace

  Memory parseMeminfo(std::string_view text) {
    Memory memory;
    std::istringstream in{std::string(text)};
    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    while (in >> key >> value) {
      std::getline(in, unit);
      if (key == "MemTotal:") {
        memory.totalKb = value;
      } else if (key == "MemAvailable:") {
        memory.availableKb = value;
      }
    }
    return memory;
  }

  std::string parseOsRelease(std::string_view text) {
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
      if (line.starts_with("PRETTY_NAME=")) {
        std::string value = line.substr(12);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
          value = value.substr(1, value.size() - 2);
        }
        return value;
      }
    }
    return {};
  }

  std::string parseCpuModel(std::string_view cpuinfo) {
    std::istringstream in{std::string(cpuinfo)};
    std::string line;
    while (std::getline(in, line)) {
      if (line.starts_with("model name")) {
        const auto colon = line.find(':');
        return colon == std::string::npos ? std::string() : trim(line.substr(colon + 1));
      }
    }
    return {};
  }

  std::vector<Disk> parseMounts(std::string_view mounts) {
    static constexpr std::array<std::string_view, 10> kRealFs{
        "ext4", "ext3", "btrfs", "xfs", "f2fs", "ntfs3", "ntfs", "fuseblk", "vfat", "exfat",
    };
    std::vector<Disk> out;
    std::istringstream in{std::string(mounts)};
    std::string device;
    std::string mount;
    std::string fs;
    std::string rest;
    while (in >> device >> mount >> fs) {
      std::getline(in, rest);
      if (!device.starts_with("/dev/") || !std::ranges::contains(kRealFs, std::string_view(fs))) {
        continue;
      }
      Disk disk{.mount = unescapeMount(mount), .device = device, .fs = fs};
      // Un dispositivo montado varias veces (subvolúmenes de btrfs): queda el montaje más corto.
      const auto it = std::ranges::find(out, device, &Disk::device);
      if (it == out.end()) {
        out.push_back(std::move(disk));
      } else if (disk.mount.size() < it->mount.size()) {
        *it = std::move(disk);
      }
    }
    return out;
  }

  std::string formatBytes(std::uint64_t bytes) {
    static constexpr std::array<const char*, 5> kUnits{"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
      value /= 1024.0;
      ++unit;
    }
    return unit == 0 ? std::format("{} {}", bytes, kUnits[unit]) : std::format("{:.1f} {}", value, kUnits[unit]);
  }

  SystemInfo collect() {
    SystemInfo info;
    info.os = parseOsRelease(readFile("/etc/os-release"));
    utsname uts{};
    if (uname(&uts) == 0) {
      info.kernel = uts.release;
      info.host = uts.nodename;
    }
    info.cpu = parseCpuModel(readFile("/proc/cpuinfo"));
    double uptime = 0.0;
    if (std::sscanf(readFile("/proc/uptime").c_str(), "%lf", &uptime) == 1) {
      info.uptimeSeconds = static_cast<std::uint64_t>(uptime);
    }
    info.memory = parseMeminfo(readFile("/proc/meminfo"));
    info.disks = parseMounts(readFile("/proc/mounts"));
    for (auto& disk : info.disks) {
      struct statvfs st{};
      if (statvfs(disk.mount.c_str(), &st) == 0) {
        disk.totalBytes = static_cast<std::uint64_t>(st.f_blocks) * st.f_frsize;
        disk.freeBytes = static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize;
      }
    }
    std::erase_if(info.disks, [](const Disk& d) { return d.totalBytes == 0; });
    std::ranges::sort(info.disks, {}, &Disk::mount);
    return info;
  }

} // namespace okaru::sysinfo
