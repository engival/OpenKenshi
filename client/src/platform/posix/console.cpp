#include "platform/console.hpp"

// Linux and macOS keep stdout / stderr on the terminal that started the client (platform/console.hpp).
namespace plat {

void setupLog(int, char**) {}
std::string logPath() { return std::string(); }

}
