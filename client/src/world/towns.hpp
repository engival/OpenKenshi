#pragma once
#include <string>
#include <vector>

struct Town
{
	std::string name;
	std::string sid;	// the TOWN record's stringID (one Town per distinct placement: several may share it)
	double x = 0, z = 0;	// world position
};

struct TownSet
{
	std::vector<Town> towns;
	double loadMs = 0;
	std::string report;		// one-line summary: placements, layer usage, disagreements

	// Loads TOWN records and leveldata townlists through kworld; one placement per town (base layer first).
	// Returns an empty set (report holds the error) on failure.
	static TownSet load(const std::string& dataDir);

	// Index of the town nearest to (x, z), or -1 when empty.
	int nearest(double x, double z) const;
};
