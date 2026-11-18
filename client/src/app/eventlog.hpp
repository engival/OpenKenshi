#pragma once
// The client's critical-events log (kenshi-k16.2.75): notable runtime events (a character snapped or caught up to the sim, the render
// clock snapped, a clip warp clamped, ...), each with the time, the sim tick and the character. A ring of the newest kCapacity events in
// memory, written beside every screenshot as <shot>.events.txt, printed by the `dump events` script op, and appended line by line to the
// run's own file (flushed per line, so a crash or a kill keeps everything up to it). docs/CLIENT.md "Critical-events log".
#include <cstdint>
#include <cstdio>
#include <string>

namespace evlog {

constexpr size_t kCapacity = 4096;

// The run's file: DIR/events-<date>-<time>-<pid>.txt; only the newest 20 run files in DIR are kept. Before it, events go to the ring only.
// first: the file's first line (k16.2.127: the build line), written before any event; "" none.
void open(const std::string& dir, const std::string& first = "");
const std::string& runFile();

// One event. kind: a short word (teleport, catchup, clock, warp, ...); tick: the render's sim tick (negative: none); id: the character's
// wire id (0: none); the rest printf-style. Thread-safe.
void add(const char* kind, double tick, uint64_t id, const char* fmt, ...) __attribute__((format(printf, 4, 5)));

// The events of the last `seconds` (<= 0: all held), oldest first, one per line, to f. Returns the count.
size_t write(FILE* f, double seconds);

// Writes path (the last `seconds`); false if it cannot be opened. count: the events written. first: the file's first line before them
// (k16.2.127: the build line); "" none.
bool writeFile(const std::string& path, double seconds, size_t& count, const std::string& first = "");

}
