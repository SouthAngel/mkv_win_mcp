#include "util.h"

#include <windows.h>

#include <cctype>

namespace util {
namespace {

const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string base64_encode(const void* data, std::size_t size) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);

    std::size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        const unsigned int v = (static_cast<unsigned int>(p[i]) << 16) |
                               (static_cast<unsigned int>(p[i + 1]) << 8) |
                               static_cast<unsigned int>(p[i + 2]);
        out.push_back(kBase64Table[(v >> 18) & 0x3F]);
        out.push_back(kBase64Table[(v >> 12) & 0x3F]);
        out.push_back(kBase64Table[(v >> 6) & 0x3F]);
        out.push_back(kBase64Table[v & 0x3F]);
    }

    const std::size_t rest = size - i;
    if (rest == 1) {
        const unsigned int v = static_cast<unsigned int>(p[i]) << 16;
        out.push_back(kBase64Table[(v >> 18) & 0x3F]);
        out.push_back(kBase64Table[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const unsigned int v = (static_cast<unsigned int>(p[i]) << 16) |
                               (static_cast<unsigned int>(p[i + 1]) << 8);
        out.push_back(kBase64Table[(v >> 18) & 0x3F]);
        out.push_back(kBase64Table[(v >> 12) & 0x3F]);
        out.push_back(kBase64Table[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int size = static_cast<int>(utf8.size());
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), size, nullptr, 0);
    if (len <= 0) {
        len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), size, nullptr, 0);
    }
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), size, out.data(), len);
    return out;
}

std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int size = static_cast<int>(wide.size());
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), size, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), size, out.data(), len, nullptr, nullptr);
    return out;
}

std::string last_error_string(unsigned long error) {
    std::string text;
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(error), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length > 0 && buffer != nullptr) {
        text = to_utf8(std::wstring(buffer, length));
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
    }
    if (buffer != nullptr) LocalFree(buffer);
    if (text.empty()) text = "error " + std::to_string(error);
    return text;
}

std::string to_lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

}  // namespace util
