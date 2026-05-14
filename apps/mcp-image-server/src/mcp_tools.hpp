#pragma once

#include "app_config.hpp"

#include "mcp_server.h"

namespace mcdk::image {

void register_image_tools(mcp::server& server, const AppConfig& config);

} // namespace mcdk::image
