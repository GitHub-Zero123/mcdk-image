# MCP 图像大模型 stdio 服务项目计划

## 1. 项目目标

开发一款基于 `libs/cpp-mcp` 的 stdio 协议 MCP 服务，使 LLM Agent 能通过 MCP 工具调用图像大模型，并在返回前可选执行像素画向处理与自审改进流程。

核心链路：

```text
LLM Agent
  -> MCP stdio server
  -> Image Model Provider
  -> 可选：临近采样压缩 / 像素画压缩
  -> 可选：透明空像素域裁剪与缩放
  -> 可选：返回 base64 给具备视觉能力的 LLM 自审
  -> 可选：轮询改进
  -> 返回最终图像与元数据
```

本阶段优先实现 OpenAI 兼容协议，`baseURL`、`apiKey`、`protocolType=openai` 等 Provider 连接配置统一通过环境变量传递；MCP 工具参数不接收这些连接配置，后续通过 Provider Adapter 扩展到其它图像模型 API。

## 2. 已有工程基础

当前工程具备以下基础：

- 根工程已设置 C++20 标准。
- MSVC 构建已配置静态运行库 MT / MTd。
- 已纳入 `libs/cpp-mcp` 子工程。
- `libs/cpp-mcp` 已提供 `mcp::server`、工具注册、stdio transport、JSON-RPC 派发能力。
- 已内置 `nlohmann/json`。
- 已内置 `stb_image.h` 与 `stb_image_write.h`，可用于 PNG/JPEG 读写与基础图像处理。

建议保持本项目为独立可执行 MCP 服务，而不是修改 `libs/cpp-mcp` 核心库；只在发现 stdio 协议兼容性或工具 schema 能力不足时再对库做最小补丁。

## 3. 技术标准与构建约束

### 3.1 语言标准

- 主工程：C++20。
- `libs/cpp-mcp` 当前内部设置 C++17，但作为子库可被 C++20 主程序链接。
- 新增业务代码全部按 C++20 编写，避免引入协程等复杂运行时特性，优先保证跨 MSVC / Clang / GCC 可移植。

### 3.2 CMake 目标

建议新增目录结构：

```text
apps/mcp-image-server/
  CMakeLists.txt
  src/
    main.cpp
    app_config.hpp
    app_config.cpp
    mcp_tools.hpp
    mcp_tools.cpp
    image_job.hpp
    image_job.cpp
    image_processor.hpp
    image_processor.cpp
    base64_image.hpp
    base64_image.cpp
    providers/
      image_provider.hpp
      openai_image_provider.hpp
      openai_image_provider.cpp
      provider_factory.hpp
      provider_factory.cpp
    net/
      http_client.hpp
      http_client.cpp
    util/
      env.hpp
      env.cpp
      file_io.hpp
      file_io.cpp
      diagnostics.hpp
      diagnostics.cpp
```

根 `CMakeLists.txt` 后续增加：

```cmake
add_subdirectory(apps/mcp-image-server)
```

应用目标建议：

```cmake
add_executable(mcp-image-server ...)
target_compile_features(mcp-image-server PRIVATE cxx_std_20)
target_link_libraries(mcp-image-server PRIVATE mcp nlohmann_json::nlohmann_json)
```

### 3.3 MSVC MT 链接

当前根工程已设置：

```cmake
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
```

新增 target 不应覆盖该设置。若后续引入外部库，必须确认外部库与 `/MT` 或 `/MTd` 一致，避免 CRT 混用。

### 3.4 HTTPS 与网络依赖

OpenAI 兼容接口通常是 HTTPS，因此需要明确 HTTP 客户端策略：

方案 A：复用 `cpp-httplib` + OpenSSL。

- 优点：已有 `httplib.h`，改动小。
- 缺点：Windows 下 OpenSSL 静态链接配置稍繁琐。
- 对应 CMake：启用 `-DMCP_SSL=ON`，并确保 OpenSSL 可发现。

方案 B：新增轻量 HTTP 抽象，底层先使用 `httplib::Client` / `httplib::SSLClient`，未来可替换 libcurl。

- 推荐采用此方案。
- 业务层只依赖 `IHttpClient`，不直接依赖具体 HTTP 库。

初版可以只支持 HTTPS OpenAI 兼容 endpoint；如未启用 SSL，应在启动时明确报错。

## 4. 总体架构

### 4.1 模块分层

```text
MCP Transport Layer
  mcp::stdio_server + mcp::server

Tool Layer
  generate_image
  edit_image / iterate_image（后续）
  process_image
  get_job_status（可选）

Application Layer
  参数校验
  任务配置归一化
  错误转换
  输出格式选择

Provider Layer
  ImageProvider 接口
  OpenAIImageProvider
  FutureProviderAdapter

Network Layer
  HttpClient 抽象
  请求头 / 超时 / 重试 / 错误码

Image Processing Layer
  PNG/JPEG decode/encode
  nearest-neighbor resize
  transparent-domain crop
  pixel-art compression

Storage Layer
  临时文件 / 输出目录
  base64 编解码
  元数据记录
```

### 4.2 核心接口设计

#### ImageProvider

```cpp
struct ImageGenerationRequest {
    std::string prompt;
    std::string model;
    std::optional<std::string> size;
    std::optional<int> n;
    std::optional<std::string> quality;
    std::optional<std::string> style;
    nlohmann::json extra;
    bool native_transparency = false;
    int timeout_seconds = 400;
};

struct ImageEditRequest {
    std::string prompt;
    std::string model;
    std::vector<ImageData> input_images;
    std::optional<std::string> size;
    std::optional<int> n;
    std::optional<std::string> quality;
    nlohmann::json extra;
    bool native_transparency = false;
    int timeout_seconds = 400;
};

struct ImageGenerationResult {
    std::vector<ImageData> images;
    nlohmann::json raw_response;
};

class ImageProvider {
public:
    virtual ~ImageProvider() = default;
    virtual ImageGenerationResult generate(const ImageGenerationRequest& request) = 0;
    virtual ImageGenerationResult edit(const ImageEditRequest& request) = 0;
};
```

#### ProviderFactory

```cpp
enum class ProviderProtocol {
    OpenAI
};

std::unique_ptr<ImageProvider> create_provider(const AppConfig& config);
```

保留扩展点：

- `openai`：OpenAI images API 兼容协议。
- `stability`：后续 Stable Diffusion API 适配。
- `comfyui`：后续本地 ComfyUI workflow 适配。
- `custom_http`：后续自定义 JSON path 映射。

## 5. 配置方案

### 5.1 配置来源优先级

Provider 连接配置只允许通过环境变量传递，不允许从 MCP tool 参数或命令行参数传递：

1. 环境变量：`MCDK_IMAGE_PROTOCOL`、`MCDK_IMAGE_BASE_URL`、`MCDK_IMAGE_API_KEY`。
2. 默认配置：仅用于非敏感、非 Provider 连接项，例如默认模型、默认超时。
3. MCP tool 参数：只允许传递单次任务参数，例如 prompt、size、n、processing、returnBase64、timeoutSeconds；涉及文件落盘时必须显式传递绝对 UTF-8 `outputPath`，严禁相对路径。
4. 命令行参数：只允许传递非敏感运行参数，例如 `--timeout-seconds`、`--log-level`；不提供 `--output-dir`、`--protocol`、`--base-url`、`--api-key`、`--api-key-env`。

### 5.2 建议命令行参数

```text
--timeout-seconds 400
--log-level info
```

### 5.3 必需/建议环境变量

```text
MCDK_IMAGE_PROTOCOL=openai
MCDK_IMAGE_BASE_URL=https://api.openai.com
MCDK_IMAGE_API_KEY=...
MCDK_IMAGE_DEFAULT_MODEL=gpt-image-1
MCDK_IMAGE_TIMEOUT_SECONDS=400
```

其中：

- `MCDK_IMAGE_PROTOCOL`：协议类型，初版固定支持 `openai`。
- `MCDK_IMAGE_BASE_URL`：OpenAI 兼容服务地址，例如 `https://api.openai.com` 或代理网关。
- `MCDK_IMAGE_API_KEY`：模型服务密钥，必须从环境变量读取。

`apiKey` 不应写入 MCP 返回内容、日志或错误详情，也不应出现在 MCP tool 入参 schema 或 Agent 配置的命令行 args 中。

## 6. MCP 工具设计

### 6.1 generate_image

用途：调用图像大模型生成图像，并可选执行图像处理与返回 base64。

输入 schema 建议：

```json
{
  "prompt": "string, required",
  "model": "string, optional",
  "size": "string, optional, example: 1024x1024",
  "n": "number, optional, default: 1",
  "quality": "string, optional",
  "style": "string, optional",
  "processing": {
    "enabled": "boolean, default: false",
    "nearestResize": {
      "enabled": "boolean",
      "targetWidth": "number",
      "targetHeight": "number"
    },
    "transparentDomainScale": {
      "enabled": "boolean",
      "padding": "number",
      "targetWidth": "number",
      "targetHeight": "number"
    },
    "pixelArtCompress": {
      "enabled": "boolean",
      "maxWidth": "number",
      "maxHeight": "number",
      "paletteLimit": "number, optional"
    }
  },
  "returnBase64": "boolean, default: true",
  "saveToFile": "boolean, default: false",
  "outputPath": "absolute UTF-8 path, required when saveToFile=true",
  "timeoutSeconds": "number, optional, default: 400"
}
```

输出内容建议：

```json
[
  {
    "type": "text",
    "text": "生成完成：1 张图像。"
  },
  {
    "type": "text",
    "text": "{...metadata...}"
  }
]
```

metadata：

```json
{
  "images": [
    {
      "index": 0,
      "mimeType": "image/png",
      "width": 512,
      "height": 512,
      "filePath": "D:/绝对路径/xxx.png",
      "base64": "optional when returnBase64=true",
      "processing": {
        "nearestResizeApplied": true,
        "transparentDomainScaleApplied": true,
        "pixelArtCompressApplied": false
      }
    }
  ],
  "provider": "openai",
  "model": "gpt-image-1",
  "elapsedMs": 12345
}
```

### 6.2 edit_image

用途：对已有图像做连续二次编辑，可用于“生成 -> 自审 -> 编辑修正 -> 再自审”的循环。输入图像来源三选一：缓存 `sourceJobId`、绝对 UTF-8 `inputPath`、或 `inputBase64`。

输入 schema 建议：

```json
{
  "prompt": "string, required, edit instruction",
  "model": "string, optional",
  "sourceJobId": "string, optional, cached generate_image/edit_image result",
  "sourceIndex": "number, optional, default: 0",
  "inputPath": "absolute UTF-8 path, optional",
  "inputBase64": "string, optional",
  "inputMimeType": "string, optional, default: image/png",
  "size": "string, optional, default: 1024x1024",
  "n": "number, optional, default: 1",
  "quality": "string, optional",
  "nativeTransparency": "boolean, optional, request alpha-capable PNG via provider parameters",
  "extra": "object, optional, provider-specific edit parameters",
  "processing": "object, optional, same as generate_image",
  "returnBase64": "boolean, default: true",
  "saveToFile": "boolean, default: false",
  "outputPath": "absolute UTF-8 path, required when saveToFile=true",
  "selfReviewHint": "boolean, default: true",
  "timeoutSeconds": "number, optional, default: 400"
}
```

输出：同 `generate_image`，返回新的 `jobId`，编辑结果进入内存缓存；确认后调用 `save_cached_image(jobId,index,outputPath)` 落盘，避免重新编辑。

### 6.3 process_image

用途：只处理本地图像或 base64 图像，不调用图像模型。便于 LLM Agent 对已有贴图、像素画素材做压缩和透明域缩放。

输入：

- `inputPath` 或 `inputBase64` 二选一；若使用 `inputPath`，必须是绝对 UTF-8 路径。
- `processing` 同 `generate_image`。
- `returnBase64`。
- `outputPath` 必填，且必须是绝对 UTF-8 路径。

### 6.4 self_review_image（可选二期）

用途：当上游 LLM 支持视觉能力时，MCP 返回 base64 与结构化审查提示，由 Agent 自己进行视觉自审。由于 MCP 服务本身未必拥有多模态 LLM，建议初版不在服务内直接调用审查 LLM，而是通过 `returnBase64=true` 与审查模板让 Agent 完成自审。

输出：

```json
{
  "reviewPrompt": "请检查该图是否满足 Minecraft 像素画/贴图需求...",
  "imageBase64": "...",
  "mimeType": "image/png"
}
```

### 6.5 iterate_image（可选二期）

用途：支持 Agent 带着上一轮自审意见再次调用生成或编辑。

输入：

- `originalPrompt`。
- `reviewFeedback`。
- `previousImagePath` / `previousImageBase64`。
- `maxIterations`，默认 1，硬上限建议 3。

注意：自动轮询可能产生费用和长时间阻塞；stdio MCP 初版建议由 Agent 控制轮询，服务只提供一次生成、一次处理、一次返回自审材料的原子能力。

## 7. OpenAI 兼容协议适配

### 7.1 Endpoint

默认：

```text
POST {baseURL}/v1/images/generations
POST {baseURL}/v1/images/edits
```

兼容项：

- `baseURL` 从环境变量 `MCDK_IMAGE_BASE_URL` 读取，如 `https://api.openai.com`、代理网关或 OpenAI-compatible 网关。
- `protocolType` 从环境变量 `MCDK_IMAGE_PROTOCOL` 读取，初版支持 `openai`。
- `apiKey` 从环境变量 `MCDK_IMAGE_API_KEY` 读取，并写入 Authorization 请求头。
- endpoint 后续可配置，初版可固定为 `/v1/images/generations`。

### 7.2 请求结构

按 OpenAI 风格组织：

```json
{
  "model": "gpt-image-1",
  "prompt": "...",
  "size": "1024x1024",
  "n": 1
}
```

不同模型对 `response_format`、`quality`、`style` 支持不一致，因此：

- 已知字段按白名单传递。
- `extra` 对象允许透传供应商扩展字段。
- 响应解析同时支持 `b64_json` 与 `url` 两类结果。
- 原生透明/半透明不通过 prompt 文本表达；tool 描述引导 Agent 使用 `nativeTransparency` 或 `extra`。
- `nativeTransparency=true` 默认只注入 `output_format="png"`，不默认注入 `background="transparent"` 或 `response_format`，避免 OpenAI-compatible 网关进入白底兼容路径；需要特殊网关字段时由 `extra` 显式覆盖。
- `edit_image` 使用 `image: [{ type: "input_image", image_url: "data:image/png;base64,..." }]` 的 JSON data URL 形式调用 `/v1/images/edits`，用于缓存图或已有图片的连续编辑。

### 7.3 响应处理

优先处理：

1. `data[].b64_json`：直接解码。
2. `data[].url`：二次 HTTP GET 下载。

若二者都不存在，返回 MCP tool error。

### 7.4 超时与重试

- 默认 `timeoutSeconds = 400`。
- 连接超时建议 30s。
- 读超时使用 `timeoutSeconds`。
- 对 429 / 5xx 可做最多 2 次指数退避重试。
- 对 400 / 401 / 403 不重试，直接返回配置或权限错误。

## 8. 图像处理计划

### 8.1 图像表示

内部使用 RGBA8：

```cpp
struct ImageBuffer {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};
```

### 8.2 临近采样缩放

用于 Minecraft 像素画/低分辨率贴图压缩：

- 不使用双线性/双三次。
- 目标尺寸通过 `targetWidth` / `targetHeight` 指定。
- 源坐标映射使用中心点或整数比例策略，保证边界稳定。

### 8.3 透明空像素域裁剪与缩放

目标：自动裁掉完全透明边界，保留主体，再缩放到目标尺寸。

流程：

1. 扫描 alpha 大于阈值的像素，得到 bounding box。
2. 根据 `padding` 扩展 bounding box。
3. 裁剪出主体区域。
4. 使用临近采样缩放到目标尺寸。
5. 如果需要固定画布，可将结果居中贴回目标画布。

参数：

- `alphaThreshold` 默认 1。
- `padding` 默认 0。
- `targetWidth` / `targetHeight` 可选。
- `anchor` 后续可支持 `center` / `bottom_center`。

### 8.4 像素画压缩

初版范围：

- 基于临近采样降低分辨率。
- 可选限制调色板数量。
- 不做复杂抖动，避免破坏 Minecraft 像素风格。

二期增强：

- K-means / Median Cut 调色板量化。
- 透明像素保护。
- Minecraft 方块色板映射。

## 9. LLM 自审与自我轮询策略

### 9.1 初版原则

MCP 服务只负责：

- 生成图像。
- 处理图像。
- 保存或返回 base64。
- 提供适合视觉 LLM 的自审 prompt。

是否自审、是否再次调用工具，由 LLM Agent 决定。

原因：

- MCP 服务不一定拥有 LLM 聊天模型能力。
- 不同 Agent 的视觉输入格式不同。
- 让 Agent 控制循环更符合 MCP 工具式设计。

### 9.2 可选自审返回

当 `returnBase64=true` 且 `selfReviewHint=true` 时，返回：

```json
{
  "selfReview": {
    "enabled": true,
    "prompt": "请作为 Minecraft 美术审查员，检查该图是否满足：清晰轮廓、透明背景、低色彩噪声、主体居中、适合贴图/像素画。若不满足，请输出下一轮改进 prompt。",
    "image": {
      "mimeType": "image/png",
      "base64": "..."
    }
  }
}
```

### 9.3 自动轮询二期

若未来服务内置审查模型或可接入聊天多模态模型，可实现：

```text
generate -> review -> refine prompt -> generate/edit -> review -> final
```

限制：

- `maxIterations <= 3`。
- 每轮记录费用相关元数据。
- 每轮都返回中间图路径，便于人工排查。
- 遇到 API 错误立即停止，不做无限循环。

## 10. 错误处理与日志

### 10.1 错误分类

- `invalid_params`：MCP 参数缺失或类型错误。
- `config_error`：环境变量中的 baseURL/apiKey/protocol 配置缺失或错误。
- `provider_auth_error`：401/403。
- `provider_rate_limited`：429。
- `provider_error`：5xx 或响应结构不兼容。
- `network_timeout`：连接或读取超时。
- `image_decode_error`：模型返回数据无法解码。
- `image_process_error`：处理参数非法或处理失败。
- `file_io_error`：输出目录不可写。

### 10.2 日志原则

- 日志写 stderr，不污染 stdout MCP 协议流。
- 永不输出 apiKey。
- provider 原始响应只在 debug 模式记录，并脱敏。
- 每个 tool call 生成 job id，便于排查。

## 11. 安全与成本控制

- `apiKey` 必须使用环境变量传递。
- `baseURL`、`apiKey`、`protocolType` 禁止通过 MCP tool 参数或命令行 args 传递。
- 禁止把 `apiKey` 放入 tool 返回。
- 限制 `n` 的最大值，初版建议 `n <= 4`。
- 限制最大输入/输出图像尺寸，避免内存爆炸。
- 限制 base64 返回大小；超过阈值时只返回文件路径。
- 所有工具文件路径必须由调用方显式传递绝对 UTF-8 路径；禁止默认输出目录、禁止相对路径、禁止在服务内把相对路径转换为绝对路径。
- 自动轮询必须有最大次数和总超时。

## 12. 测试计划

### 12.1 单元测试

- 配置解析。
- OpenAI 响应解析：`b64_json`、`url`、错误响应。
- base64 decode/encode。
- PNG decode/encode。
- 临近采样缩放。
- 透明 bounding box 裁剪。
- 参数校验。

### 12.2 集成测试

- 使用 mock HTTP server 模拟 OpenAI 响应。
- MCP stdio 初始化、`tools/list`、`tools/call`。
- 生成后处理并保存文件。
- 超时与错误码映射。

### 12.3 手工验收

使用 MCP Inspector 或 Agent 配置 stdio server：

```json
{
  "mcpServers": {
    "mcdk-image": {
      "command": "path/to/mcp-image-server.exe",
      "args": [
        "--timeout-seconds", "400"
      ],
      "env": {
        "MCDK_IMAGE_PROTOCOL": "openai",
        "MCDK_IMAGE_BASE_URL": "https://api.openai.com",
        "MCDK_IMAGE_API_KEY": "${OPENAI_API_KEY}"
      }
    }
  }
}
```

验收场景：

1. Agent 调用 `generate_image` 生成 1 张 PNG。
2. 开启 `processing.nearestResize` 输出 16x16 / 32x32 像素画。
3. 开启 `transparentDomainScale` 裁掉透明边框并居中。
4. 开启 `returnBase64` 后，Agent 能进行视觉自审。
5. 超时设置为 400s 时长任务不被 300s 提前中断。

## 13. 里程碑

### M0：项目骨架

目标：可编译、可启动、可完成 MCP stdio 初始化。

任务：

- 新增 `apps/mcp-image-server`。
- 接入 `mcp::server` 与 `mcp::stdio_server`。
- 注册 `generate_image` 占位工具。
- CMake 接入主工程。
- 确认 MSVC MT 构建通过。

产出：

- `mcp-image-server` 可执行文件。
- `tools/list` 能看到图像工具。

### M1：OpenAI Provider MVP

目标：能调用 OpenAI 兼容图像生成接口并保存图片。

任务：

- 实现 `AppConfig`，并从环境变量读取 `MCDK_IMAGE_PROTOCOL`、`MCDK_IMAGE_BASE_URL`、`MCDK_IMAGE_API_KEY`。
- 实现 `HttpClient`。
- 实现 `OpenAIImageProvider`。
- 支持 `b64_json` 响应。
- 支持输出 PNG 文件。
- 默认超时调整为 400s。

产出：

- `generate_image` 可真实生成并保存图片。

### M2：图像处理 MVP

目标：支持 Minecraft 像素画常用压缩处理。

任务：

- 实现 RGBA8 图像载入/保存。
- 实现临近采样缩放。
- 实现透明域裁剪与缩放。
- 实现 `process_image` 工具。
- 在 `generate_image` 后串联 processing pipeline。

产出：

- 能输出固定尺寸、透明背景、像素风的 PNG。

### M3：自审支持

目标：让具备视觉能力的 LLM Agent 能拿到 base64 并自审。

任务：

- 实现 `returnBase64`。
- 实现 `selfReviewHint` 返回结构。
- 增加 base64 大小限制。
- 增加自审 prompt 模板。

产出：

- Agent 可基于返回图像进行视觉自审并再次调用工具。

### M4：稳健性与扩展

目标：提高生产可用性，并为未来 provider 扩展做准备。

任务：

- 支持 `url` 响应下载。
- 支持重试与错误分类。
- 增加 mock 测试。
- 增加 provider factory。
- 增加结构化日志与 job id。

产出：

- OpenAI 兼容 provider 稳定可用。
- 新 provider 可按接口快速接入。

### M5：自动轮询二期

目标：在 Agent 外部控制基础上，探索服务内自动迭代能力。

任务：

- 设计 `iterate_image`。
- 限制 `maxIterations`。
- 支持根据审查反馈合成下一轮 prompt。
- 记录每轮中间产物。

产出：

- 可选自动改进链路，但默认关闭。

## 14. 推荐实施顺序

1. 先实现 stdio MCP 可执行骨架。
2. 再实现配置解析与 OpenAI provider。
3. 然后实现图片保存和 base64 返回。
4. 再实现临近采样和透明域缩放。
5. 最后实现自审返回结构与 Agent 轮询文档。

不建议一开始就做服务内自动轮询；应先保证原子工具稳定、可组合、可被 Agent 明确控制。

## 15. 初版完成定义

满足以下条件即可视为 MVP 完成：

- 使用 C++20 编译通过。
- MSVC 下使用 MT / MTd 运行库。
- 可作为 stdio MCP server 被 Agent 拉起。
- 支持通过环境变量传递 `protocol=openai`、`baseURL`、`apiKey`。
- `generate_image` 能调用 OpenAI 兼容 API。
- 默认模型调用超时为 400s。
- 能保存 PNG 文件。
- 能可选返回 base64。
- 能可选执行临近采样压缩。
- 能可选执行透明空像素域裁剪与缩放。
- 错误不会污染 stdout MCP 协议流。
