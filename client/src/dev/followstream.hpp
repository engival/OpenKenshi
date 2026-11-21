#pragma once
// Side-by-side follow mode (docs/FOLLOW_STREAM.md, kenshi-cym.40.2.5): kclient connects out to the inspector agent's stream and applies what the
// original's player does (camera, pause / speed, selection, orders), keeps the original's character positions (dev/origstore.hpp), and locks the
// player's own camera and mouse orders while it follows. Dev only: the code is dev/followstream.cpp, the game calls the hooks in devhooks.hpp.
#include <functional>
#include <string>

class SimView;

namespace dev
{
	// What the follow mode needs of the game (main.cpp fills it once, right after dev::init).
	struct FollowHost
	{
		std::function<SimView&()> sim;
		std::function<bool()> worldOn;	// the first world frame has been drawn (the CAMERA_START camera is in place)
		std::function<void()> beginSession;	// the imported world is in: the title goes, the world is the player's (worldChosen)
		std::function<void(double yaw, double pitch, double dist, const double focus[3])> camSet;	// the RTS camera: pivot, yaw / pitch (rad), orbit distance; ends auto-center and any follow
		std::function<void(double& yaw, double& pitch, double& dist, double focus[3])> camGet;
		std::function<void(int floor)> setFloor;	// buildings.restoreView(floor, 0)
		std::function<void(const std::string&)> toast;
		std::string clientHash;
	};
}
