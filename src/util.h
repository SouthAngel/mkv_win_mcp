#pragma once

#include <cstddef>
#include <string>

namespace util {

// 标准 Base64 编码（带 '=' 填充）
std::string base64_encode(const void* data, std::size_t size);

// UTF-8 与 UTF-16 互转
std::wstring to_wide(const std::string& utf8);
std::string to_utf8(const std::wstring& wide);

// 将 GetLastError() 的返回值转成可读文本
std::string last_error_string(unsigned long error);

// 字符串工具
std::string to_lower(std::string text);
std::string trim(const std::string& text);

}  // namespace util
