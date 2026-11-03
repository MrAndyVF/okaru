#pragma once

// OKARU: información básica del sistema para el módulo Home de los ajustes.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace okaru::sysinfo {

  struct Memory {
    std::uint64_t totalKb = 0;
    std::uint64_t availableKb = 0;
  };

  struct Disk {
    std::string mount;
    std::string device;
    std::string fs;
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
  };

  struct SystemInfo {
    std::string os;     // PRETTY_NAME de /etc/os-release
    std::string kernel; // uname -r
    std::string host;
    std::string cpu;
    std::uint64_t uptimeSeconds = 0;
    Memory memory;
    std::vector<Disk> disks;
  };

  [[nodiscard]] Memory parseMeminfo(std::string_view text);
  [[nodiscard]] std::string parseOsRelease(std::string_view text);
  [[nodiscard]] std::string parseCpuModel(std::string_view cpuinfo);
  // Montajes de discos reales (sin pseudo-sistemas, un montaje por dispositivo: el más corto).
  [[nodiscard]] std::vector<Disk> parseMounts(std::string_view mounts);
  [[nodiscard]] std::string formatBytes(std::uint64_t bytes);

  [[nodiscard]] SystemInfo collect();

} // namespace okaru::sysinfo
