#include "ksim/simdata.hpp"

#include <filesystem>
#include <mutex>

namespace ksim {

namespace {

std::mutex gMutex;
std::string gExplicit;

std::string exeSimData()
{
	std::error_code ec;
	const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec); // Linux; the other platforms: kenshi-379.19
	return ec ? std::string() : (exe.parent_path() / "simdata").string();
}

bool usable(const std::string& dir)
{
	std::error_code ec;
	return !dir.empty() && std::filesystem::is_regular_file(dir + "/ksim_ai_behaviours.tsv", ec)
		&& std::filesystem::is_directory(dir + "/behaviour", ec);
}

#ifdef KSIM_SIMDATA_BAKED
const std::string kBaked = KSIM_SIMDATA_BAKED;
#else
const std::string kBaked;
#endif

} // namespace

void setSimDataDir(std::string dir)
{
	const std::lock_guard<std::mutex> lock(gMutex);
	gExplicit = std::move(dir);
}

std::string simDataDir()
{
	const std::lock_guard<std::mutex> lock(gMutex);
	if (!gExplicit.empty())
		return gExplicit;
	if (const std::string beside = exeSimData(); usable(beside))
		return beside;
	return usable(kBaked) ? kBaked : std::string();
}

std::string simDataError()
{
	const std::string dir = simDataDir();
	{
		const std::lock_guard<std::mutex> lock(gMutex);
		if (!gExplicit.empty())
			return usable(gExplicit) ? std::string()
				: "--sim-data " + gExplicit + " lacks behaviour/ or ksim_ai_behaviours.tsv";
	}
	if (!dir.empty())
		return {};
	std::string message = "no sim data directory (behaviour/ and ksim_ai_behaviours.tsv): looked for --sim-data DIR (not given) and for ";
	const std::string beside = exeSimData();
	message += beside.empty() ? std::string("simdata/ beside the executable (its path is unknown)") : beside;
	return message;
}

} // namespace ksim
