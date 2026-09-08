#pragma once

#include <string>
#include <vector>

namespace qr {

// Payload text of every QR code found in imageBytes. Never throws.
std::vector<std::string> decode(const std::string& imageBytes);

}  // namespace qr
