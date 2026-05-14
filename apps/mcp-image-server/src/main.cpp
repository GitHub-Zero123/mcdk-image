#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "app_config.hpp"
#include "mcp_tools.hpp"

#include "mcp_server.h"
#include "mcp_stdio_server.h"

#include <exception>
#include <iostream>

namespace {

void setup_console_encoding() {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
}

} // namespace

int main(int argc, char** argv) {
    setup_console_encoding();

    try {
        mcdk::image::AppConfig config = mcdk::image::load_app_config(argc, argv);

        mcp::server::configuration server_config;
        server_config.name = "mcdk-image";
        server_config.version = "0.1.0";

        mcp::server server(server_config);
        server.set_server_info("mcdk-image", "0.1.0");
        mcdk::image::register_image_tools(server, config);

        mcp::stdio_server stdio(server);
        stdio.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "mcdk-image fatal: " << e.what() << std::endl;
        return 1;
    }
}
