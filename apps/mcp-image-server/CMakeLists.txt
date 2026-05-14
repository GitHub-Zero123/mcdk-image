add_executable(mcp-image-server
    src/main.cpp
    src/app_config.cpp
    src/http_client.cpp
    src/openai_image_provider.cpp
    src/image_processor.cpp
    src/mcp_tools.cpp
)

target_compile_features(mcp-image-server PRIVATE cxx_std_20)

target_include_directories(mcp-image-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/libs/stb
)

target_link_libraries(mcp-image-server PRIVATE
    mcp
    nlohmann_json::nlohmann_json
)

if(MCP_SSL)
    target_compile_definitions(mcp-image-server PRIVATE MCP_SSL CPPHTTPLIB_OPENSSL_SUPPORT)
endif()

if(MSVC)
    target_compile_options(mcp-image-server PRIVATE /utf-8 /EHsc)
endif()
