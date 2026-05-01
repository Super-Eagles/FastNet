/**
 * @file base64.h
 * @brief Base64 helpers for FastNet
 */
#pragma once

#include "Config.h"

#include <string>
#include <string_view>

namespace FastNet {

FASTNET_API std::string base64Encode(const Buffer& data);
FASTNET_API std::string base64Encode(std::string_view data);

FASTNET_API bool tryBase64Decode(std::string_view encoded, Buffer& output);
FASTNET_API bool tryBase64DecodeToString(std::string_view encoded, std::string& output);

FASTNET_API Buffer base64Decode(std::string_view encoded);
FASTNET_API std::string base64DecodeToString(std::string_view encoded);

} // namespace FastNet
