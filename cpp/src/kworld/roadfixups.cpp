#include "kworld/roadfixups.hpp"

#include "kworld/error.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace kworld {

namespace {

constexpr int kVersion = 1;
constexpr double kGuard = 0.5; // xz tolerance of the old_x / old_z guard (the writer rounds to 0.01)
constexpr const char* kHeader = "road\top\tindex\tx\ty\tz\told_x\told_z\tstatus\tnote";

const char* opName(FixupOp op)
{
	switch (op)
	{
	case FixupOp::Move: return "move";
	case FixupOp::Insert: return "insert";
	case FixupOp::Delete: return "delete";
	default: return "mark";
	}
}

std::vector<std::string_view> split(std::string_view line)
{
	std::vector<std::string_view> cells;
	for (std::size_t from = 0;;)
	{
		const std::size_t tab = line.find('\t', from);
		cells.push_back(line.substr(from, tab == std::string_view::npos ? tab : tab - from));
		if (tab == std::string_view::npos)
			return cells;
		from = tab + 1;
	}
}

[[noreturn]] void fail(int lineNo, const std::string& what) { throw Error("road fixups: line " + std::to_string(lineNo) + ": " + what); }

double number(std::string_view cell, int lineNo, const char* column)
{
	const std::string text(cell);
	char* tail = nullptr;
	const double value = std::strtod(text.c_str(), &tail);
	if (text.empty() || *tail != '\0' || !std::isfinite(value))
		fail(lineNo, std::string("bad number in column ") + column + ": '" + text + "'");
	return value;
}

double polyline(const std::vector<RoadNetwork::Marker>& m)
{
	double len = 0.0;
	for (std::size_t i = 0; i + 1 < m.size(); ++i)
		len += std::hypot(m[i + 1].x - m[i].x, m[i + 1].z - m[i].z);
	return len;
}

std::uint64_t fnv(std::string_view bytes)
{
	std::uint64_t hash = 14695981039346656037ull;
	for (const char c : bytes)
	{
		hash ^= static_cast<std::uint8_t>(c);
		hash *= 1099511628211ull;
	}
	return hash;
}

std::string readText(std::string_view path)
{
	std::ifstream in{std::string(path), std::ios::binary};
	if (!in)
		throw Error("road fixups: cannot read " + std::string(path));
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

RoadFixup parseRow(std::string_view line, int lineNo)
{
	const std::vector<std::string_view> c = split(line);
	if (c.size() < 9 || c.size() > 10)
		fail(lineNo, "expected 9 or 10 columns, got " + std::to_string(c.size()));
	RoadFixup row;
	row.road = std::string(c[0]);
	if (row.road.empty())
		fail(lineNo, "empty road id");
	if (c[1] == "move") row.op = FixupOp::Move;
	else if (c[1] == "insert") row.op = FixupOp::Insert;
	else if (c[1] == "delete") row.op = FixupOp::Delete;
	else if (c[1] == "mark") row.op = FixupOp::Mark;
	else fail(lineNo, "unknown op '" + std::string(c[1]) + "'");
	if (!c[2].empty())
	{
		row.index = static_cast<int>(number(c[2], lineNo, "index"));
		if (row.index < 0)
			fail(lineNo, "negative index");
	}
	else if (row.op != FixupOp::Mark)
		fail(lineNo, "empty index on a " + std::string(opName(row.op)) + " row");
	if (row.op == FixupOp::Move || row.op == FixupOp::Insert)
	{
		row.x = number(c[3], lineNo, "x");
		row.y = number(c[4], lineNo, "y");
		row.z = number(c[5], lineNo, "z");
	}
	if (!c[6].empty() || !c[7].empty())
	{
		row.hasOld = true;
		row.oldX = number(c[6], lineNo, "old_x");
		row.oldZ = number(c[7], lineNo, "old_z");
	}
	if ((row.op == FixupOp::Move || row.op == FixupOp::Delete) && !row.hasOld)
		fail(lineNo, "a " + std::string(opName(row.op)) + " row needs old_x and old_z");
	row.status = std::string(c[8]);
	if (row.status != "ok" && row.status != "unsure" && row.status != "bad")
		fail(lineNo, "unknown status '" + row.status + "'");
	if (c.size() > 9)
		row.note = std::string(c[9]);
	return row;
}

} // namespace

RoadFixupTable parseRoadFixups(std::string_view text)
{
	RoadFixupTable table;
	bool versioned = false, headed = false;
	int lineNo = 0;
	for (std::size_t from = 0; from < text.size();)
	{
		const std::size_t nl = text.find('\n', from);
		std::string_view line = text.substr(from, nl == std::string_view::npos ? nl : nl - from);
		from = nl == std::string_view::npos ? text.size() : nl + 1;
		++lineNo;
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		if (line.empty())
			continue;
		if (line[0] == '#')
		{
			constexpr std::string_view tag = "# kenshi-road-fixups ";
			if (!versioned && line.substr(0, tag.size()) == tag)
			{
				table.version = static_cast<int>(number(line.substr(tag.size()), lineNo, "version"));
				if (table.version != kVersion)
					fail(lineNo, "format version " + std::to_string(table.version) + " is not supported (this build reads " + std::to_string(kVersion) + ")");
				versioned = true;
			}
			continue;
		}
		if (!versioned)
			fail(lineNo, "the first line must be '# kenshi-road-fixups " + std::to_string(kVersion) + "'");
		if (!headed)
		{
			headed = true;
			if (line.substr(0, 5) != "road\t")
				fail(lineNo, "expected the header row");
			continue;
		}
		try { table.rows.push_back(parseRow(line, lineNo)); }
		catch (const Error& e) { table.errors.push_back(e.what()); }
	}
	if (!versioned)
		throw Error("road fixups: no '# kenshi-road-fixups " + std::to_string(kVersion) + "' line");
	return table;
}

std::vector<RoadFixupResult> applyRoadFixups(std::vector<RoadNetwork::Road>& roads, const std::vector<RoadFixup>& rows)
{
	using Marker = RoadNetwork::Marker;
	std::vector<RoadFixupResult> results(rows.size());
	std::map<std::string, std::size_t> byId;
	for (std::size_t r = 0; r < roads.size(); ++r)
		byId[roads[r].sid] = r;
	// Per road: what each original marker became (a move, or removal), and what is inserted after it. Rows of one road are judged on the
	// original list, so they are independent of each other and of file order.
	struct Edit
	{
		bool moved = false, removed = false;
		Marker to;
		std::vector<Marker> after;
	};
	std::map<std::size_t, std::vector<Edit>> edits;
	std::map<std::size_t, std::vector<Marker>> originals;
	for (std::size_t i = 0; i < rows.size(); ++i)
	{
		const RoadFixup& row = rows[i];
		RoadFixupResult& result = results[i];
		const auto found = byId.find(row.road);
		if (found == byId.end())
		{
			result.why = "no such road";
			continue;
		}
		const std::size_t r = found->second;
		const RoadNetwork::Road& road = roads[r];
		const std::vector<Marker>& base = originals.try_emplace(r, road.original.empty() ? road.markers : road.original).first->second;
		const int count = static_cast<int>(base.size());
		if (row.op == FixupOp::Mark && row.index < 0)
		{
			result.applied = true;
			continue;
		}
		if (row.index >= count)
		{
			result.why = "index " + std::to_string(row.index) + " out of range (road has " + std::to_string(count) + " markers)";
			continue;
		}
		if (row.hasOld && (std::fabs(base[row.index].x - row.oldX) > kGuard || std::fabs(base[row.index].z - row.oldZ) > kGuard))
		{
			result.why = "guard mismatch: marker is at (" + std::to_string(base[row.index].x) + ", " + std::to_string(base[row.index].z) + ")";
			continue;
		}
		std::vector<Edit>& list = edits.try_emplace(r, std::vector<Edit>(count)).first->second;
		Edit& edit = list[row.index];
		result.before = base[row.index];
		if (row.op == FixupOp::Mark)
		{
			result.applied = true;
			continue;
		}
		if (row.op == FixupOp::Insert)
		{
			if (row.index + 1 >= count)
			{
				result.why = "insert after the last marker";
				continue;
			}
			edit.after.push_back({row.x, row.y, row.z, road.width});
			result.after = edit.after.back();
			result.applied = true;
			continue;
		}
		if (edit.moved || edit.removed)
		{
			result.why = "marker already edited by another row";
			continue;
		}
		if (row.op == FixupOp::Delete)
		{
			if (row.index == 0 || row.index + 1 == count)
			{
				result.why = "a road's end marker cannot be deleted";
				continue;
			}
			edit.removed = true;
		} else {
			edit.moved = true;
			edit.to = {row.x, row.y, row.z, road.width};
			result.after = edit.to;
		}
		result.applied = true;
	}
	// Build the patched marker lists; the length results of a road's rows are the road's before / after.
	std::map<std::size_t, std::pair<double, double>> lengths;
	for (auto& [r, list] : edits)
	{
		RoadNetwork::Road& road = roads[r];
		const std::vector<Marker>& base = originals[r];
		std::vector<Marker> patched;
		bool changed = false;
		for (std::size_t m = 0; m < base.size(); ++m)
		{
			const Edit& edit = list[m];
			changed = changed || edit.moved || edit.removed || !edit.after.empty();
			if (!edit.removed)
				patched.push_back(edit.moved ? edit.to : base[m]);
			patched.insert(patched.end(), edit.after.begin(), edit.after.end());
		}
		if (!changed)
			continue;
		const double before = polyline(base);
		const double after = polyline(patched);
		lengths[r] = {road.length, before > 0.0 ? road.length * after / before : after};
		road.original = base;
		road.markers = std::move(patched);
		road.length = lengths[r].second;
		road.patched = true;
	}
	for (std::size_t i = 0; i < rows.size(); ++i)
	{
		const auto road = byId.find(rows[i].road);
		if (road == byId.end() || !results[i].applied)
			continue;
		const auto len = lengths.find(road->second);
		if (len != lengths.end() && rows[i].op != FixupOp::Mark)
		{
			results[i].lengthBefore = len->second.first;
			results[i].lengthAfter = len->second.second;
		}
		else
			results[i].lengthBefore = results[i].lengthAfter = roads[road->second].length;
	}
	return results;
}

std::string writeRoadFixups(const RoadFixupTable& table)
{
	std::vector<const RoadFixup*> rows;
	for (const RoadFixup& row : table.rows)
		rows.push_back(&row);
	std::stable_sort(rows.begin(), rows.end(), [](const RoadFixup* a, const RoadFixup* b)
	{
		return a->road != b->road ? a->road < b->road : a->index < b->index;
	});
	std::string out = "# kenshi-road-fixups " + std::to_string(kVersion) + "\n" + kHeader + "\n";
	char buf[64];
	const auto num = [&](double v)
	{
		std::snprintf(buf, sizeof buf, "%.2f", v);
		return std::string(buf);
	};
	for (const RoadFixup* row : rows)
	{
		const bool pos = row->op == FixupOp::Move || row->op == FixupOp::Insert;
		std::string note = row->note;
		for (char& c : note)
			if (c == '\t' || c == '\n' || c == '\r')
				c = ' ';
		out += row->road + '\t' + opName(row->op) + '\t' + (row->index >= 0 ? std::to_string(row->index) : std::string()) + '\t';
		out += pos ? num(row->x) + '\t' + num(row->y) + '\t' + num(row->z) + '\t' : "\t\t\t";
		out += row->hasOld ? num(row->oldX) + '\t' + num(row->oldZ) + '\t' : "\t\t";
		out += row->status + '\t' + note + '\n';
	}
	return out;
}

std::uint64_t roadFixupsHash(std::string_view path)
{
	std::error_code error;
	if (path.empty() || !std::filesystem::is_regular_file(std::string(path), error))
		return 0;
	const std::string text = readText(path);
	try { return parseRoadFixups(text).rows.empty() ? 0 : fnv(text); }
	catch (const Error&) { return 0; } // a bad table loads unpatched
}

std::uint64_t foldRoadFixups(std::uint64_t dataHash, std::string_view path)
{
	const std::uint64_t table = roadFixupsHash(path);
	return table == 0 ? dataHash : (dataHash ^ table) * 1099511628211ull;
}

std::string defaultRoadFixupsPath()
{
	std::error_code error;
	if (const char* env = std::getenv("KSIM_ROAD_FIXUPS"))
		return env;
#ifdef KSIM_ROAD_FIXUPS_DEFAULT
	if (std::filesystem::is_regular_file(KSIM_ROAD_FIXUPS_DEFAULT, error))
		return KSIM_ROAD_FIXUPS_DEFAULT;
#endif
	return {};
}

} // namespace kworld
