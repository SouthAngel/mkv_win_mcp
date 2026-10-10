#pragma once

#include <string>

#include <nlohmann/json.hpp>

// 执行一步键盘动作，返回该步的描述文本。action 是去掉 "key." 前缀后的名字。
// 动作不属于键盘或参数非法时抛 std::runtime_error。
std::string run_keyboard_step(const std::string& action, const nlohmann::json& args);
