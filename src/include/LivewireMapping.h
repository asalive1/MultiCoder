#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace livewire {

/// Convert Livewire channel N (0..65535) to multicast IP "239.192.A.B"
/// where A = N/256, B = N%256.
std::optional<std::string> channelToMulticast(uint32_t channel);

/// Convert multicast IP "239.192.A.B" back to channel N = A*256+B.
std::optional<uint32_t> multicastToChannel(const std::string& ip);

/// RTP port used by Axia Livewire (always 5004).
constexpr uint16_t LIVEWIRE_RTP_PORT = 5004;

} // namespace livewire
