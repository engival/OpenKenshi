#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The original's iteration order of a GameData per-type index (kenshi-inspect findings/newgame_startorder_static.md section 2, kenshi-rwh.72, and
// rwh101_idle_pick_static.md section 3 for the rehash, kenshi-rwh.101): the container keeps each type in a boost::unordered_map<stringID, GameData*> and
// every list the exe builds from it (the new-game starts, the ANIMATION manager's idle list) is that map's walk. Our own port of the findings' reference
// implementation; nothing here is the exe's code.
//
// Feed the sids in merge order (GameData::byType's stable order: gamedata.base, data/*.mod, then the mods.cfg list; a repeated sid is a merge, never a second
// row). The map is constructed with B = 16 buckets (load factor 1.0); a sid's bucket is its hash & (B - 1); a new bucket's first record becomes the walk's head,
// a later record of a bucket goes in front of that bucket's first. Adding the n+1-th record when n+1 exceeds the threshold (B) first rehashes (140411c30): the new
// B is the next power of two of floor(max(1.5 n, n + 1)) + 1 (14040dd60), so 16 -> 32 at the 17th record, 64 at the 33rd, 128 at the 65th (the 126 stock
// ANIMATION sids end at B = 128). The rehash (140411a10) walks the old list: a node whose new bucket is empty stays where it is, one whose new bucket
// already has records is unlinked and put in front of them. UNSURE: the rehash relink is the findings' description, checked only through the stock ANIMATION
// set (the guard5 / guard4 main / guard 1h positions of the findings' table, `dump anim`).
namespace sidorder
{
	inline std::uint64_t hash(const std::string& sid)	// 14006a6f0
	{
		std::uint64_t h = 0;
		for (const unsigned char c : sid) { h = h ^ ((std::uint64_t)c + (h >> 2) + h * 0x40 + 0x9e3779b9); }	// 64-bit wraparound
		h = h * 0x200000 + ~h;
		h = ((h >> 0x18) ^ h) * 0x109;
		h = ((h >> 0x0e) ^ h) * 0x15;
		h = ((h >> 0x1c) ^ h) * 0x80000001;
		return h;
	}

	// The walk: the indices into `sids` in the original's order (a repeated sid is skipped).
	inline std::vector<std::size_t> walk(const std::vector<std::string>& sids)
	{
		struct Node { std::size_t sid = 0; std::uint64_t hash = 0; int next = -1; };
		std::vector<Node> nodes(1);	// nodes[0]: the sentinel before every bucket's first element
		std::size_t buckets = 16;
		std::vector<int> before(buckets, -1);	// per bucket: the node before its first element, -1 = empty
		std::vector<std::string> seen;
		auto link = [&](int at)	// a record into its bucket
		{
			const std::size_t b = (std::size_t)(nodes[(size_t)at].hash & (buckets - 1));
			if (before[b] < 0)
			{
				before[b] = 0;
				if (nodes[0].next >= 0) { before[(std::size_t)(nodes[(size_t)nodes[0].next].hash & (buckets - 1))] = at; }	// the old head's predecessor is now this node
				nodes[(size_t)at].next = nodes[0].next;
				nodes[0].next = at;
			}
			else
			{
				const int p = before[b];
				nodes[(size_t)at].next = nodes[(size_t)p].next;
				nodes[(size_t)p].next = at;
			}
		};
		std::size_t count = 0;
		for (std::size_t i = 0; i < sids.size(); i++)
		{
			bool dup = false;
			for (const std::string& s : seen) { if (s == sids[i]) { dup = true; break; } }
			if (dup) { continue; }
			seen.push_back(sids[i]);
			if (count + 1 > buckets)	// grow before the new node is linked
			{
				const std::size_t want = (std::size_t)((double)count * 1.5 > (double)(count + 1) ? (double)count * 1.5 : (double)(count + 1)) + 1;
				std::size_t nb = 4;
				while (nb < want) { nb *= 2; }
				buckets = nb;
				before.assign(buckets, -1);
				int prev = 0;	// the relink: a node whose bucket is empty stays; else it goes in front of that bucket's records
				while (nodes[(size_t)prev].next >= 0)
				{
					const int n = nodes[(size_t)prev].next;
					const std::size_t b = (std::size_t)(nodes[(size_t)n].hash & (buckets - 1));
					if (before[b] < 0) { before[b] = prev; prev = n; }
					else
					{
						nodes[(size_t)prev].next = nodes[(size_t)n].next;
						nodes[(size_t)n].next = nodes[(size_t)before[b]].next;
						nodes[(size_t)before[b]].next = n;
					}
				}
			}
			nodes.push_back({ i, hash(sids[i]), -1 });
			link((int)nodes.size() - 1);
			count++;
		}
		std::vector<std::size_t> out;
		for (int at = nodes[0].next; at >= 0; at = nodes[(size_t)at].next) { out.push_back(nodes[(size_t)at].sid); }
		return out;
	}
}
