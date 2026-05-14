#include "../include/LivewireMapping.h"
#include <sstream>
#include <regex>

using namespace std;
namespace livewire {

static bool validMulticastPrefix(const std::string& ip) {
    return ip.rfind("239.192.", 0) == 0;
}

std::optional<std::string> channelToMulticast(uint32_t channel) {
    if (channel > 65535) return {};
    uint32_t a = (channel / 256) & 0xFF;
    uint32_t b = channel % 256;
    std::ostringstream ss;
    ss << "239.192." << a << "." << b;
    return ss.str();
}

std::optional<uint32_t> multicastToChannel(const std::string& ip) {
    if (!validMulticastPrefix(ip)) return {};
    std::regex re(R"(^239\.192\.(\d{1,3})\.(\d{1,3})$)");
    std::smatch m;
    if (!std::regex_match(ip, m, re)) return {};
    int a = stoi(m[1].str());
    int b = stoi(m[2].str());
    if (a < 0 || a > 255 || b < 0 || b > 255) return {};
    uint32_t n = a * 256 + b;
    return n;
}

}
