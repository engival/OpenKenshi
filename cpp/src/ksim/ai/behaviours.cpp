#include "ksim/ai/behaviours.hpp"

#include "ksim/ai/blackboard.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace ksim {

namespace {

// Any time past 0 h: a timed signal package reads the board's contract expiry (0 here), so it is finished at every later hour.
constexpr double kPickHours = 1.0;

// UnloadedPlatoonJob in enum order (KenshiLib Faction.h; counts agree with
// ORIGINAL_AI.md: NONE 79, GOHOME 61, TRAVEL_TARGET 44, FAST 21, SHORTRANGE 15).
constexpr std::array<std::string_view, 7> kUnloadedNames = {
	"NONE", "PATROL_TOWN", "PATROL_SHORTRANGE", "PATROL_LONGRANGE", "GOHOME",
	"TRAVEL_TARGET", "TRAVEL_TARGET_FAST"};

std::string lower(std::string_view text)
{
	std::string out(text);
	for (char& c : out)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

bool startsWith(std::string_view text, std::string_view prefix)
{
	return text.substr(0, prefix.size()) == prefix;
}

std::string_view unloadedName(std::int32_t value)
{
	if (value < 0 || static_cast<std::size_t>(value) >= kUnloadedNames.size())
		return "NONE";
	return kUnloadedNames[static_cast<std::size_t>(value)];
}

// The package's ancestors, nearest first, depth first, each once.
void collectAncestors(const flecs::entity package, std::vector<flecs::entity>& out,
	std::set<flecs::entity_t>& seen)
{
	std::vector<flecs::entity> parents;
	package.each<tmpl::AiInherits>([&parents](flecs::entity parent)
	{
		parents.push_back(parent);
	});
	for (const flecs::entity parent : parents)
	{
		if (!seen.insert(parent.id()).second)
			continue;
		out.push_back(parent);
		collectAncestors(parent, out, seen);
	}
}

const std::string& nameOf(flecs::entity prefab)
{
	return prefab.get<tmpl::Source>()->name;
}

} // namespace

std::string_view behaviourName(Behaviour behaviour) noexcept
{
	switch (behaviour)
	{
	case Behaviour::Fallback:
		return "fallback";
	case Behaviour::Travel:
		return "travel";
	case Behaviour::GoHome:
		return "go_home";
	case Behaviour::StayHome:
		return "stay_home";
	case Behaviour::PatrolLocal:
		return "patrol_local";
	case Behaviour::AnimalTerritory:
		return "animal_territory";
	case Behaviour::Raid:
		return "raid";
	case Behaviour::ManTheGate:
		return "man_the_gate";
	case Behaviour::TradeCircuit:
		return "trade_circuit";
	case Behaviour::ShopKeeper:
		return "shop_keeper";
	case Behaviour::ResidentLife:
		return "resident_life";
	case Behaviour::BarHangout:
		return "bar_hangout";
	case Behaviour::GuardShift:
		return "guard_shift";
	case Behaviour::CageWarden:
		return "cage_warden";
	}
	return "fallback";
}

std::vector<BehaviourRule> BehaviourRegistry::readRules(const std::string& path)
{
	std::ifstream in(path);
	if (!in)
		throw std::runtime_error("cannot open behaviour table " + path);
	std::vector<BehaviourRule> rules;
	std::string line;
	std::size_t number = 0;
	while (std::getline(in, line))
	{
		++number;
		if (line.empty() || line.front() == '#')
			continue;
		std::vector<std::string> fields;
		std::stringstream stream(line);
		std::string field;
		while (std::getline(stream, field, '\t'))
			fields.push_back(field);
		if (fields.size() < 3 || (fields[0] != "package" && fields[0] != "unloaded"
			&& fields[0] != "keyword") || fields[1].empty() || fields[2].empty())
			throw std::runtime_error(path + ":" + std::to_string(number)
				+ ": expected match<TAB>key<TAB>behaviour[<TAB>note]");
		rules.push_back(BehaviourRule{fields[0], fields[1], fields[2],
			fields.size() > 3 ? fields[3] : std::string()});
	}
	return rules;
}

Behaviour BehaviourRegistry::implemented(std::string_view name) const
{
	for (int i = 1; i < kBehaviourCount; ++i)
	{
		if (behaviourName(static_cast<Behaviour>(i)) == name)
			return static_cast<Behaviour>(i);
	}
	return Behaviour::Fallback;
}

Mapping BehaviourRegistry::resolvePackage(const World&, flecs::entity package) const
{
	Mapping mapping;
	mapping.package = package.get<tmpl::Source>()->stringID;
	mapping.packageName = nameOf(package);

	std::vector<flecs::entity> ancestors;
	std::set<flecs::entity_t> seen{package.id()};
	collectAncestors(package, ancestors, seen);

	const auto apply = [&](const BehaviourRule& rule)
	{
		mapping.wanted = rule.behaviour;
		mapping.rule = rule.match;
		mapping.key = rule.key;
		mapping.behaviour = implemented(rule.behaviour);
		mapping.fallback = mapping.behaviour == Behaviour::Fallback
			? state::AiReason::Planned : state::AiReason::None;
	};

	const std::string name = lower(mapping.packageName);
	for (const BehaviourRule& rule : rules_)
	{
		if (rule.match == "package" && lower(rule.key) == name)
		{
			apply(rule);
			return mapping;
		}
	}

	std::string_view unloaded = unloadedName(package.get<tmpl::AiPackage>()->unloadedFunc);
	for (std::size_t i = 0; unloaded == "NONE" && i < ancestors.size(); ++i)
		unloaded = unloadedName(ancestors[i].get<tmpl::AiPackage>()->unloadedFunc);
	for (const BehaviourRule& rule : rules_)
	{
		if (rule.match == "unloaded" && rule.key == unloaded)
		{
			apply(rule);
			return mapping;
		}
	}

	std::vector<std::string> names{name};
	for (const flecs::entity ancestor : ancestors)
	{
		if (!startsWith(lower(nameOf(ancestor)), "addon"))
			names.push_back(lower(nameOf(ancestor)));
	}
	for (const std::string& candidate : names)
	{
		for (const BehaviourRule& rule : rules_)
		{
			if (rule.match == "keyword"
				&& (rule.key == "*" || candidate.find(lower(rule.key)) != std::string::npos))
			{
				apply(rule);
				return mapping;
			}
		}
	}

	mapping.rule = "none";
	return mapping;
}

BehaviourRegistry::BehaviourRegistry(const World& world, std::vector<BehaviourRule> rules)
	: rules_(std::move(rules))
{
	none_.rule = "none";
	for (const auto& [sid, prefab] : world.prefabs())
	{
		if (prefab.has<tmpl::AiPackage>())
			packages_.emplace(sid, resolvePackage(world, prefab));
	}
	for (const auto& [sid, prefab] : world.prefabs())
	{
		if (!prefab.has<tmpl::SquadTemplate>())
			continue;
		// The G1 board's pick (dt19): the template's `AI packages` registered in data order, the ladder run from the highest priority.
		// The same functions the squad's Blackboard runs (Simulation::ensureBlackboard), so a template-level pick and a built squad's
		// agree; a trade circuit or a bar hangout wins by priority, not by an override.
		std::vector<std::pair<std::int32_t, flecs::entity>> linked; // (data order, package)
		prefab.each<tmpl::AiPackageLink>([&](flecs::entity package)
		{
			linked.emplace_back(prefab.get<tmpl::AiPackageLink>(package)->order, package);
		});
		std::sort(linked.begin(), linked.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
		blackboard::Board board;
		bool priorityZero = false;
		for (const auto& [order, package] : linked)
		{
			const std::string& packageSid = package.get<tmpl::Source>()->stringID;
			const auto known = packages_.find(packageSid);
			if (known == packages_.end())
				continue;
			blackboard::Package entry;
			entry.sid = packageSid;
			entry.priority = prefab.get<tmpl::AiPackageLink>(package)->priority;
			entry.signalFunc = package.get<tmpl::AiPackage>()->signalFunc;
			std::int32_t first = INT32_MAX;
			package.each<tmpl::AiLeadsTo>([&](flecs::entity next)
			{
				if (const std::int32_t at = package.get<tmpl::AiLeadsTo>(next)->order; at < first)
				{
					first = at;
					entry.leadsTo = next.get<tmpl::Source>()->stringID;
				}
			});
			priorityZero = priorityZero || entry.priority == 0;
			blackboard::registerPackage(board, std::move(entry));
		}
		blackboard::resolveLeads(board);
		blackboard::tick(board, kPickHours);
		Mapping chosen = board.current.empty() ? none_ : packages_.at(board.current);
		// The faction's `AI fallback` is registered after the template's packages at priority 0: it applies only when the ladder reaches
		// priority 0 with no template package there (a template package at 0 that has finished drops that level, fallback included).
		if (board.current.empty() && !priorityZero)
			openToFallback_.insert(sid);
		squads_.emplace(sid, std::move(chosen));
	}
}

const Mapping& BehaviourRegistry::squad(std::string_view templateSid, std::string_view fallbackPackage) const
{
	const std::string key(templateSid);
	const auto found = squads_.find(key);
	if (found == squads_.end())
		return none_;
	if (found->second.package.empty() && !fallbackPackage.empty() && openToFallback_.count(key) != 0)
		return package(fallbackPackage);
	return found->second;
}

const Mapping& BehaviourRegistry::package(std::string_view packageSid) const
{
	const auto found = packages_.find(std::string(packageSid));
	return found == packages_.end() ? none_ : found->second;
}

Coverage BehaviourRegistry::coverage(const flecs::world& ecs) const
{
	Coverage report;
	std::map<std::string, CoverageRow> rows;
	for (const auto& [sid, mapping] : packages_)
		rows[sid] = CoverageRow{sid, mapping.packageName, mapping, {}, 0, 0};
	for (const auto& [sid, mapping] : squads_)
	{
		if (!mapping.package.empty())
			++rows[mapping.package].squadTemplates;
	}

	std::map<std::string, std::size_t> wanted;
	ecs.each([&](const state::SquadInstance& squad)
	{
		const Mapping& mapping = this->squad(squad.templateSid);
		++report.squads;
		if (mapping.package.empty())
			++report.squadsNoPackage;
		else
			++rows[mapping.package].squads;
		if (mapping.behaviour != Behaviour::Fallback)
			++report.squadsReal;
		else if (mapping.fallback == state::AiReason::Planned)
			++report.squadsPlanned;
		else
			++report.squadsUnmapped;
		++wanted[mapping.wanted.empty() ? "(none)" : mapping.wanted];
	});

	for (auto& [sid, row] : rows)
	{
		++report.packagesTotal;
		if (row.mapping.behaviour != Behaviour::Fallback)
			++report.packagesReal;
		else if (row.mapping.fallback == state::AiReason::Planned)
			++report.packagesPlanned;
		else
			++report.packagesUnmapped;
		report.packages.push_back(std::move(row));
	}
	std::sort(report.packages.begin(), report.packages.end(),
		[](const CoverageRow& left, const CoverageRow& right)
		{
			return std::tie(right.squads, left.name, left.package)
				< std::tie(left.squads, right.name, right.package);
		});
	report.byBehaviour.assign(wanted.begin(), wanted.end());
	std::sort(report.byBehaviour.begin(), report.byBehaviour.end(),
		[](const auto& left, const auto& right)
		{
			return left.second != right.second ? left.second > right.second
				: left.first < right.first;
		});
	return report;
}

void printCoverage(std::ostream& out, const Coverage& coverage, std::size_t topPackages)
{
	const auto percent = [&coverage](std::size_t count)
	{
		return coverage.squads == 0 ? 0.0
			: 100.0 * static_cast<double>(count) / static_cast<double>(coverage.squads);
	};
	out << "squads " << coverage.squads << ": real behaviour " << coverage.squadsReal
		<< " (" << percent(coverage.squadsReal) << "%), planned (fallback) "
		<< coverage.squadsPlanned << " (" << percent(coverage.squadsPlanned)
		<< "%), unmapped (fallback) " << coverage.squadsUnmapped << " ("
		<< percent(coverage.squadsUnmapped) << "%), of which no package at all "
		<< coverage.squadsNoPackage << "\n";
	out << "packages " << coverage.packagesTotal << ": real " << coverage.packagesReal
		<< ", planned " << coverage.packagesPlanned << ", unmapped "
		<< coverage.packagesUnmapped << "\n";
	out << "squads by wanted behaviour:";
	for (const auto& [name, count] : coverage.byBehaviour)
		out << " " << name << "=" << count;
	out << "\nsquads  templates  status    behaviour        rule:key  routine  package\n";
	std::size_t shown = 0;
	for (const CoverageRow& row : coverage.packages)
	{
		if (shown++ >= topPackages || (row.squads == 0 && topPackages != SIZE_MAX))
			break;
		const char* status = row.mapping.behaviour != Behaviour::Fallback ? "real"
			: row.mapping.fallback == state::AiReason::Planned ? "planned" : "unmapped";
		out << row.squads << "\t" << row.squadTemplates << "\t" << status << "\t"
			<< (row.mapping.wanted.empty() ? "(fallback)" : row.mapping.wanted) << "\t"
			<< row.mapping.rule << (row.mapping.key.empty() ? "" : ":" + row.mapping.key)
			<< "\t" << (row.routine.empty() ? "-" : row.routine) << "\t" << row.name << "\n";
	}
}

} // namespace ksim
