#pragma once

namespace okaru::ipc {

  // Entry point for `okaru msg <command> [args...]`. Returns a process exit
  // code. Forwards the command to the running instance over the IPC socket.
  int runCli(int argc, char* argv[]);

} // namespace okaru::ipc
