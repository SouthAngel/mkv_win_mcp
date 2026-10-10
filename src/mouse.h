#pragma once

#include <string>

#include <nlohmann/json.hpp>

// 执行一步鼠标动作，返回该步的描述文本。action 是去掉 "mouse." 前缀后的名字。
// 动作不属于鼠标或参数非法时抛 std::runtime_error。
std::string run_mouse_step(const std::string& action, const nlohmann::json& args);
