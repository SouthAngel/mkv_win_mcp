#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// 一个 MCP 工具：名称、描述、入参 schema、处理函数
struct Tool {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    // 处理函数返回 tools/call 结果中的 content 数组；出错时抛 std::runtime_error
    std::function<nlohmann::json(const nlohmann::json& args)> handler;
};

// 已注册的全部工具
const std::vector<Tool>& all_tools();

// 按名称查找，未找到返回 nullptr
const Tool* find_tool(const std::string& name);

// 构造 tools/call 的纯文本 content
inline nlohmann::json text_content(const std::string& text) {
    return nlohmann::json::array({{{"type", "text"}, {"text", text}}});
}

// 工具入参读取
namespace jargs {

inline bool has(const nlohmann::json& args, const char* key) {
    auto it = args.find(key);
    return it != args.end() && !it->is_null();
}

inline int get_int(const nlohmann::json& args, const char* key, int fallback) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return fallback;
    if (it->is_number_integer()) return it->get<int>();
    if (it->is_number_float()) return static_cast<int>(it->get<double>());
    throw std::runtime_error(std::string("argument '") + key + "' must be an integer");
}

inline double get_double(const nlohmann::json& args, const char* key, double fallback) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return fallback;
    if (it->is_number()) return it->get<double>();
    throw std::runtime_error(std::string("argument '") + key + "' must be a number");
}

inline bool get_bool(const nlohmann::json& args, const char* key, bool fallback) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    throw std::runtime_error(std::string("argument '") + key + "' must be a boolean");
}

inline std::string get_string(const nlohmann::json& args, const char* key, const std::string& fallback) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return fallback;
    if (it->is_string()) return it->get<std::string>();
    throw std::runtime_error(std::string("argument '") + key + "' must be a string");
}

}  // namespace jargs
