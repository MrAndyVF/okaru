#pragma once

#include <filesystem>
#include <string>

namespace okaru::util {

  // Lowercase hex MD5 of a file's contents. Empty string if the file cannot be read.
  [[nodiscard]] std::string fileMd5Hex(const std::filesystem::path& path);

} // namespace okaru::util
