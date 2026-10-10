#include <nlohmann/json.hpp>

#include <windows.h>
#include <gdiplus.h>

#include <fcntl.h>
#include <io.h>

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "tools.h"
#include "util.h"

namespace {

using json = nlohmann::json;

const char* const kProtocolVersion = "2025-06-18";
const char* const kServerName = "mkv-win-mcp";
const char* const kServerVersion = "1.0.0";

const char* const kInstructions =
    "Controls a Windows desktop. Recommended loop: call 'screenshot' to see the current screen, "
    "then act with 'mouse_key' (a sequence of mouse and keyboard actions), then screenshot again to verify "
    "the result. "
    "All coordinates are absolute virtual-screen pixels with the origin at the top-left, matching the image "
    "returned by 'screenshot'. User interface privileges apply: input cannot be injected into a window that "
    "runs elevated while this process is not.";

void log_line(const std::string& message) {
    std::cerr << "[" << kServerName << "] " << message << std::endl;
}

json make_result(const json& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json make_error(const json& id, int code, const std::string& message) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

// 使用每显示器 DPI 感知，保证截图坐标与鼠标坐标一致（均为物理像素）
void enable_dpi_awareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using SetDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto set_context = reinterpret_cast<SetDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4))
#endif
        if (set_context != nullptr && set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

json handle_tools_call(const json& id, const json& params) {
    auto name_it = params.find("name");
    if (name_it == params.end() || !name_it->is_string()) {
        return make_error(id, -32602, "Invalid params: 'name' must be a string");
    }

    const std::string name = name_it->get<std::string>();
    const Tool* tool = find_tool(name);
    if (tool == nullptr) {
        return make_error(id, -32602, "Unknown tool: " + name);
    }

    json args = json::object();
    auto it = params.find("arguments");
    if (it != params.end()) {
        if (!it->is_object()) {
            return make_error(id, -32602, "arguments must be an object");
        }
        args = *it;
    }

    try {
        return make_result(id, {{"content", tool->handler(args)}, {"isError", false}});
    } catch (const std::exception& error) {
        log_line(std::string("tool '") + name + "' failed: " + error.what());
        return make_result(id,
                           {{"content", text_content(std::string("Error: ") + error.what())},
                            {"isError", true}});
    }
}

// 处理单条 JSON-RPC 消息；通知（无 id）返回 nullopt
std::optional<json> handle_message(const json& request) {
    if (!request.is_object()) {
        return make_error(json(), -32600, "Invalid Request: expected a JSON object");
    }

    const bool is_notification = !request.contains("id") || request["id"].is_null();
    const json id = is_notification ? json() : request["id"];

    // method 类型不符时不能走 value()，那会抛 type_error 被兜成 -32603
    auto method_it = request.find("method");
    if (method_it == request.end() || !method_it->is_string()) {
        if (is_notification) return std::nullopt;
        return make_error(id, -32600, "Invalid Request: 'method' must be a string");
    }
    const std::string method = method_it->get<std::string>();

    json params = json::object();
    auto params_it = request.find("params");
    if (params_it != request.end() && params_it->is_object()) {
        params = *params_it;
    }

    if (is_notification) {
        if (method == "notifications/initialized") {
            log_line("client initialized");
        } else if (method == "notifications/cancelled") {
            log_line("client cancelled a request");
        }
        return std::nullopt;
    }

    if (method == "initialize") {
        std::string protocol = kProtocolVersion;
        auto version_it = params.find("protocolVersion");
        if (version_it != params.end() && version_it->is_string()) {
            protocol = version_it->get<std::string>();
        }
        return make_result(id, {{"protocolVersion", protocol},
                                {"capabilities", {{"tools", {{"listChanged", false}}}}},
                                {"serverInfo", {{"name", kServerName}, {"version", kServerVersion}}},
                                {"instructions", kInstructions}});
    }

    if (method == "ping") {
        return make_result(id, json::object());
    }

    if (method == "tools/list") {
        json tools = json::array();
        for (const Tool& tool : all_tools()) {
            tools.push_back({{"name", tool.name},
                             {"description", tool.description},
                             {"inputSchema", tool.input_schema}});
        }
        return make_result(id, {{"tools", tools}});
    }

    if (method == "tools/call") {
        return handle_tools_call(id, params);
    }

    if (method == "resources/list") {
        return make_result(id, {{"resources", json::array()}});
    }

    if (method == "prompts/list") {
        return make_result(id, {{"prompts", json::array()}});
    }

    return make_error(id, -32601, "Method not found: " + method);
}

}  // namespace

int main() {
    // MCP stdio 传输：stdout 只能承载 JSON-RPC，日志一律走 stderr
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::ios::sync_with_stdio(false);

    enable_dpi_awareness();

    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        log_line("GdiplusStartup failed; screenshot tool will not work");
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.empty()) continue;

        json request = json::parse(line, nullptr, false);
        if (request.is_discarded()) {
            log_line("received invalid JSON, responding with a parse error");
            std::cout << make_error(json(), -32700, "Parse error").dump() << "\n" << std::flush;
            continue;
        }

        std::vector<json> batch;
        if (request.is_array()) {
            batch.assign(request.begin(), request.end());
        } else {
            batch.push_back(std::move(request));
        }

        for (const json& message : batch) {
            std::optional<json> response;
            try {
                response = handle_message(message);
            } catch (const std::exception& error) {
                log_line(std::string("internal error: ") + error.what());
                response = make_error(message.is_object() && message.contains("id")
                                          ? message["id"]
                                          : json(),
                                      -32603, std::string("Internal error: ") + error.what());
            }
            if (response.has_value()) {
                std::cout << response->dump() << "\n" << std::flush;
            }
        }
    }

    if (gdiplus_token != 0) Gdiplus::GdiplusShutdown(gdiplus_token);
    return 0;
}
