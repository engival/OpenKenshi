// kfollowparse_test: the follow stream's grammar, records and the original's position store, without a game (docs/FOLLOW_STREAM.md, kenshi-cym.40.2.5).
#include <cmath>
#include <cstdio>
#include <string>

#include "dev/followparse.hpp"
#include "dev/origstore.hpp"

using namespace follow;

static int fails = 0;
static void check(bool ok, const char* what) { if (!ok) { fprintf(stderr, "kfollowparse_test FAIL: %s\n", what); fails++; } }
static Status P(const std::string& l, Record& r) { return parse(l, r); }

int main()
{
	Record r;
	Line l;
	check(tokenize("order kind=move ids=1,2 to=1,2,3", l) && l.verb == "order" && l.kv.size() == 3, "tokenize plain");
	check(tokenize("order what=\"a \\\"b\\\" c\\\\d\" x=1", l) && *l.get("what") == "a \"b\" c\\d" && *l.get("x") == "1", "quoted value with escapes");
	check(!tokenize("order what=\"open", l), "unterminated quote");
	check(!tokenize("order what=\"a\"b", l), "text after a closing quote");
	check(!tokenize("order  a=1", l), "double space");
	check(!tokenize("order a=1 ", l), "trailing space");
	check(!tokenize("order a", l), "token without =");
	check(tokenize("select ids=", l) && l.get("ids") && l.get("ids")->empty(), "empty value");
	check(quote("a b") == "\"a b\"" && quote("x") == "x" && quote("q\"") == "\"q\\\"\"", "quote");

	check(P("hello v=1", r) == Status::kOk && r.type == Type::kHello && r.v == 1, "hello");
	check(P("hello v=1 future=7 other=\"x y\"", r) == Status::kOk, "unknown keys skipped");
	check(P("frobnicate a=1", r) == Status::kUnknown, "unknown verb skipped");
	check(P("frobnicate \"", r) == Status::kUnknown, "garbled unknown verb skipped");
	check(P("align t=123.5", r) == Status::kOk && r.t == 123.5, "align");
	check(P("align t=abc", r) == Status::kMalformed, "align bad number");
	check(P("align t=nan", r) == Status::kMalformed && P("align t=inf", r) == Status::kMalformed, "nan / inf refused");
	check(P("align t=1e2", r) == Status::kOk && r.t == 100, "exponent");

	check(P("cam pos=1.000,2.000,3.000 rot=1.000000,0,0,0 zoom=150.000 floor=2", r) == Status::kOk && r.floor == 2 && r.zoom == 150 && r.pos[2] == 3 && r.rot[0] == 1, "cam");
	check(P("cam pos=1,2 rot=1,0,0,0 zoom=150 floor=2", r) == Status::kMalformed, "cam short pos");
	check(P("cam pos=1,2,3 rot=1,0,0,0 zoom=-150 floor=2", r) == Status::kMalformed, "cam negative zoom");
	check(P("cam pos=1,2,3 rot=1,0,0,0 zoom=150 floor=4", r) == Status::kMalformed, "cam floor 4");
	check(P("cam pos=1,2,3 rot=1,0,0,0 zoom=150", r) == Status::kMalformed, "cam missing floor");
	check(P("cam pos=1,2,x rot=1,0,0,0 zoom=150 floor=0", r) == Status::kMalformed, "cam bad pos");
	check(P("cam pos=1,2,3 rot=2,0,0,0 zoom=150 floor=0", r) == Status::kMalformed, "cam non-unit rot");
	check(P("cam pos=1,2,3,4 rot=1,0,0,0 zoom=150 floor=0", r) == Status::kMalformed, "cam long pos");

	check(P("speed v=0", r) == Status::kOk && r.speed == 0, "speed 0");
	check(P("speed v=3", r) == Status::kOk && r.speed == 3, "speed 3");
	check(P("speed v=-1", r) == Status::kMalformed, "speed negative");
	check(P("select ids=5,6,7", r) == Status::kOk && r.ids.size() == 3 && r.ids[2] == 7, "select");
	check(P("select ids=", r) == Status::kOk && r.ids.empty(), "empty select");
	check(P("select ids=5,,7", r) == Status::kMalformed, "empty id item");
	check(P("select", r) == Status::kMalformed, "select without ids");

	check(P("order kind=move ids=1,2 to=10,20,30 queue=1 src=29", r) == Status::kOk && r.kind == Kind::kMove && r.queue && r.src == 29 && r.pos[1] == 20 && r.ids.size() == 2, "order move");
	check(P("order kind=move ids=1 src=29", r) == Status::kMalformed, "move without to");
	check(P("order kind=move ids= to=1,2,3", r) == Status::kMalformed, "move with empty ids");
	check(P("order kind=attack ids=1 target=9 src=5", r) == Status::kOk && r.kind == Kind::kAttack && r.target == 9, "order attack");
	check(P("order kind=attack ids=1 src=5", r) == Status::kMalformed, "attack without target");
	check(P("order kind=interact ids=1 target=9 action=5 src=26", r) == Status::kOk && r.kind == Kind::kInteract && r.action == 5, "order interact");
	check(P("order kind=unmapped ids=1,2 what=\"issue_task type=12 target=Foo\"", r) == Status::kOk && r.kind == Kind::kUnmapped && r.what == "issue_task type=12 target=Foo", "order unmapped");
	check(P("order kind=teleport ids=1", r) == Status::kOk && r.kind == Kind::kUnmapped && r.what.find("kind=teleport") == 0, "an unknown kind is logged as unmapped");
	check(P("order ids=1", r) == Status::kMalformed, "order without kind");

	check(P("orig id=77 pos=1,2,3 yaw=0.5 t=100.25 state=ko", r) == Status::kOk && r.target == 77 && r.state == State::kKo && r.t == 100.25, "orig");
	check(P("orig id=77 pos=1,2,3 yaw=0.5 t=100.25", r) == Status::kOk && r.state == State::kUp, "orig no state");
	check(P("orig id=77 pos=1,2,3 yaw=0.5 t=1 state=flying", r) == Status::kMalformed, "orig bad state");
	check(P("clock t=48.5 day=3 hours=0.5", r) == Status::kOk && r.t == 48.5 && r.day == 3, "clock");

	// the store
	OrigStore s;
	OrigSample a, b, o;
	a.t = 1.0; a.pos[0] = 0; a.yaw = 3.0;
	b.t = 2.0; b.pos[0] = 10; b.yaw = -3.0;
	s.put(5, b);
	s.put(5, a);	// out of order
	check(s.latest(5) && s.latest(5)->t == 2.0, "latest after an out-of-order put");
	check(s.at(5, 1.5, o) && std::fabs(o.pos[0] - 5) < 1e-9, "interpolated position");
	check(std::fabs(std::fabs(o.yaw) - M_PI) < 0.2 && std::fabs(std::remainder(o.yaw - M_PI, 2 * M_PI)) < 0.2, "yaw the short way");
	check(s.at(5, 2.0 + 3.0 / 3600, o) && o.pos[0] == 10, "held past the end within kHold");
	check(!s.at(5, 2.0 + 60.0 / 3600, o), "no answer far past the end");
	check(!s.at(6, 1.5, o), "unknown id");
	for (int k = 0; k < 20; k++) { OrigSample x; x.t = 10 + k; s.put(9, x); }
	check(s.ids().size() == 2 && !s.at(9, 10.0, o) && s.at(9, 29.0, o), "history is bounded");
	s.clear();
	check(s.size() == 0 && !s.latest(5), "clear");

	if (fails) { fprintf(stderr, "kfollowparse_test: %d failed\n", fails); return 1; }
	printf("kfollowparse_test ok\n");
	return 0;
}
