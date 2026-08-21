#pragma once

#include "types.h"

#include <optional>
#include <string>

namespace ustb {

std::optional<uint64_t> extract_u64_field(const std::string& html, const char* key);
std::optional<std::string> extract_quoted_field(const std::string& html,
                                                const char* key);
PortalInfo parse_portal_html(const std::string& html);

}  // namespace ustb
