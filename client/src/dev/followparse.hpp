#pragma once
// The follow stream's line grammar and records (docs/FOLLOW_STREAM.md sections 2, 4; kenshi-cym.40.2.5). Pure parsing, no game: dev/followstream.cpp
// applies what comes out, tests/followstream/parse_test.cpp checks it without a game. A known record that is malformed is skipped whole (status
// kMalformed, the caller logs it once per verb); an unknown verb or key is skipped (kUnknown / ignored).
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace follow
{
	constexpr size_t kMaxLine = 4096;	// bytes, the '\n' included (section 2): a longer line is dropped by the reader

	struct Line
	{
		std::string verb;
		std::vector<std::pair<std::string, std::string>> kv;	// in order; a quoted value is already unescaped
		const std::string* get(std::string_view key) const;	// the last such key, or nullptr
	};

	// verb + key=value tokens split at single spaces; a value starting with '"' runs to the closing quote (\\ and \" escape). False: a token with no '=',
	// an unterminated quote, text after a closing quote that is not a space, an empty line, or a double space.
	bool tokenize(std::string_view line, Line& out);

	enum class Type { kHello, kAlign, kCam, kSpeed, kSelect, kOrder, kOrig, kClock };
	enum class Status { kOk, kUnknown, kMalformed };
	enum class Kind { kMove, kAttack, kInteract, kUnmapped };
	enum class State : uint8_t { kUp, kDown, kKo, kDead, kCarried };

	struct Record
	{
		Type type = Type::kHello;
		int v = 0;	// hello
		double t = 0;	// align, orig, clock: the original's game hours
		double pos[3] = {};	// cam (pivot), order move (to), orig
		double rot[4] = {};	// cam: w, x, y, z
		double zoom = 0;	// cam: positive
		int floor = 0;	// cam
		double speed = 0;	// speed
		std::vector<uint64_t> ids;	// select, order
		Kind kind = Kind::kUnmapped;	// order
		bool queue = false;
		uint64_t target = 0;	// order attack / interact; orig: the id
		int action = 0;	// order interact
		int src = -1;	// order: the original's task type, -1 absent
		std::string what;	// order unmapped
		double yaw = 0;	// orig
		State state = State::kUp;	// orig
		double day = 0, hours = 0;	// clock
	};

	// One line (no newline) to a record. kUnknown: an unknown verb (or one the client never receives). kMalformed: a known verb whose line breaks the
	// grammar or lacks / garbles a required key; why says what (when given).
	Status parse(std::string_view line, Record& out, std::string* why = nullptr);

	// Number helpers (strict: the whole text, finite).
	bool parseDouble(std::string_view s, double& out);
	bool parseId(std::string_view s, uint64_t& out);
	bool parseIds(std::string_view s, std::vector<uint64_t>& out);	// comma separated, no empty item; "" is the empty set
	bool parseVec(std::string_view s, double* out, int n);	// n comma separated numbers

	// Quote a value for a record we send (spaces, quotes, backslashes).
	std::string quote(std::string_view v);
}
