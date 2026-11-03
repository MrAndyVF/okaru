#pragma once

namespace okaru::config {

  // Entry point for `okaru config <command> [options]`. Returns a process
  // exit code. Pure CLI helper; does not start Application or mutate live config.
  int runCli(int argc, char* argv[]);

} // namespace okaru::config
