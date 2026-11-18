#pragma once
#include <cstdint>
#include <string>

namespace kutil {

// Crash recorder: on SIGSEGV / SIGBUS / SIGFPE / SIGILL / SIGABRT the program writes the signal, the tick, a backtrace and the memory
// map to stderr and to DIR/crash-<time>-<pid>.txt, then dies with the signal as before. `program` is the name the lines carry and the
// binary addr2line is pointed at. Shared by kclient (tick = the client's frame) and ksim_server (tick = the sim's tick: the number a
// replay names). Resolve a frame `program() [0xADDR]` with `addr2line -f -C -e program 0xADDR` (a build with -g; not PIE, so the
// address is the binary's own).
void crashlog_install(const std::string& dir, const std::string& program);
void crashlog_tick(std::uint64_t tick); // the program's counter, reported in the crash file

} // namespace kutil
