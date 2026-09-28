#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "keyboard.h"
#include "util.h"

namespace {

void sleep_ms(int milliseconds) {
    if (milliseconds > 0) std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

void flush_inputs(std::vector<INPUT>& inputs) {
    if (inputs.empty()) return;
    const UINT total = static_cast<UINT>(inputs.size());
    const UINT sent = SendInput(total, inputs.data(), sizeof(INPUT));
    inputs.clear();
    if (sent == total) return;

    // SendInput 会被 UIPI 截断，返回 0 < sent < total；此时不能当成功，
    // 否则组合键可能只落下 modifier，把 Ctrl 之类留在按下状态。
    const std::string reason = util::last_error_string(GetLastError());
    if (sent == 0) {
        throw std::runtime_error("SendInput failed: " + reason +
                                 " (the desktop may be locked or a higher-integrity window has focus)");
    }
    throw std::runtime_error("SendInput injected only " + std::to_string(sent) + " of " +
                             std::to_string(total) + " events: " + reason +
                             " (a key or button may be left in the pressed state)");
}

// 方向键、编辑键等属于扩展键，必须带上 KEYEVENTF_EXTENDEDKEY
bool is_extended_key(WORD vk) {
    switch (vk) {
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_NUMLOCK:
        case VK_SNAPSHOT:
        case VK_DIVIDE:
        case VK_APPS:
        case VK_RWIN:
            return true;
        default:
            return false;
    }
}

bool is_modifier(WORD vk) {
    switch (vk) {
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
            return true;
        default:
            return false;
    }
}

const std::unordered_map<std::string, WORD>& key_table() {
    static const std::unordered_map<std::string, WORD> table = [] {
        std::unordered_map<std::string, WORD> keys = {
            {"ctrl", VK_CONTROL},       {"control", VK_CONTROL},
            {"lctrl", VK_LCONTROL},     {"leftctrl", VK_LCONTROL},
            {"rctrl", VK_RCONTROL},     {"rightctrl", VK_RCONTROL},
            {"shift", VK_SHIFT},        {"lshift", VK_LSHIFT},
            {"rshift", VK_RSHIFT},      {"alt", VK_MENU},
            {"lalt", VK_LMENU},         {"ralt", VK_RMENU},
            {"win", VK_LWIN},           {"lwin", VK_LWIN},
            {"rwin", VK_RWIN},          {"meta", VK_LWIN},
            {"super", VK_LWIN},         {"cmd", VK_LWIN},
            {"enter", VK_RETURN},       {"return", VK_RETURN},
            {"esc", VK_ESCAPE},         {"escape", VK_ESCAPE},
            {"tab", VK_TAB},            {"space", VK_SPACE},
            {"spacebar", VK_SPACE},     {"backspace", VK_BACK},
            {"back", VK_BACK},          {"delete", VK_DELETE},
            {"del", VK_DELETE},         {"insert", VK_INSERT},
            {"ins", VK_INSERT},         {"home", VK_HOME},
            {"end", VK_END},            {"pageup", VK_PRIOR},
            {"pgup", VK_PRIOR},         {"pagedown", VK_NEXT},
            {"pgdn", VK_NEXT},          {"up", VK_UP},
            {"down", VK_DOWN},          {"left", VK_LEFT},
            {"right", VK_RIGHT},        {"arrowup", VK_UP},
            {"arrowdown", VK_DOWN},     {"arrowleft", VK_LEFT},
            {"arrowright", VK_RIGHT},   {"capslock", VK_CAPITAL},
            {"numlock", VK_NUMLOCK},    {"scrolllock", VK_SCROLL},
            {"printscreen", VK_SNAPSHOT}, {"prtsc", VK_SNAPSHOT},
            {"prtscr", VK_SNAPSHOT},    {"pause", VK_PAUSE},
            {"apps", VK_APPS},          {"volumeup", VK_VOLUME_UP},
            {"volumedown", VK_VOLUME_DOWN}, {"volumemute", VK_VOLUME_MUTE},
            {"playpause", VK_MEDIA_PLAY_PAUSE}, {"nexttrack", VK_MEDIA_NEXT_TRACK},
            {"prevtrack", VK_MEDIA_PREV_TRACK}, {"semicolon", VK_OEM_1},
            {"equal", VK_OEM_PLUS},     {"comma", VK_OEM_COMMA},
            {"minus", VK_OEM_MINUS},    {"period", VK_OEM_PERIOD},
            {"slash", VK_OEM_2},        {"backslash", VK_OEM_5},
            {"grave", VK_OEM_3},        {"leftbracket", VK_OEM_4},
            {"rightbracket", VK_OEM_6}, {"apostrophe", VK_OEM_7},
        };
        for (char c = 'a'; c <= 'z'; ++c) {
            keys[std::string(1, c)] = static_cast<WORD>(std::toupper(static_cast<unsigned char>(c)));
        }
        for (char c = '0'; c <= '9'; ++c) {
            keys[std::string(1, c)] = static_cast<WORD>(c);
        }
        for (int i = 1; i <= 24; ++i) {
            keys["f" + std::to_string(i)] = static_cast<WORD>(VK_F1 + i - 1);
        }
        return keys;
    }();
    return table;
}

// 把键名解析成虚拟键码，符号键需要的修饰键追加到 extra_modifiers
WORD resolve_key(const std::string& raw_name, std::vector<WORD>& extra_modifiers) {
    const std::string name = util::to_lower(util::trim(raw_name));
    if (name.empty()) {
        throw std::runtime_error("key name must not be empty");
    }

    const auto& table = key_table();
    auto it = table.find(name);
    if (it != table.end()) return it->second;

    if (name.size() == 1) {
        const SHORT scan = VkKeyScanW(static_cast<wchar_t>(name[0]));
        if (scan != -1) {
            const BYTE state = HIBYTE(scan);
            if (state & 1) extra_modifiers.push_back(VK_SHIFT);
            if (state & 2) extra_modifiers.push_back(VK_CONTROL);
            if (state & 4) extra_modifiers.push_back(VK_MENU);
            return static_cast<WORD>(LOBYTE(scan));
        }
    }

    throw std::runtime_error("unknown key name: '" + raw_name + "'");
}

void append_key_event(std::vector<INPUT>& inputs, WORD vk, bool key_up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = (is_extended_key(vk) ? KEYEVENTF_EXTENDEDKEY : 0) | (key_up ? KEYEVENTF_KEYUP : 0);
    inputs.push_back(input);
}

void append_unicode_event(std::vector<INPUT>& inputs, wchar_t character, bool key_up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = character;
    input.ki.dwFlags = KEYEVENTF_UNICODE | (key_up ? KEYEVENTF_KEYUP : 0);
    inputs.push_back(input);
}

void tap_key(WORD vk) {
    std::vector<INPUT> inputs;
    append_key_event(inputs, vk, false);
    append_key_event(inputs, vk, true);
    flush_inputs(inputs);
}

void type_text(const std::string& utf8, int interval_ms) {
    const std::wstring wide = util::to_wide(utf8);
    if (wide.empty()) {
        throw std::runtime_error("'text' must not be empty");
    }

    std::vector<INPUT> inputs;
    inputs.reserve(64);

    for (const wchar_t character : wide) {
        if (character == L'\r') continue;
        if (character == L'\n') {
            append_key_event(inputs, VK_RETURN, false);
            append_key_event(inputs, VK_RETURN, true);
        } else if (character == L'\t') {
            append_key_event(inputs, VK_TAB, false);
            append_key_event(inputs, VK_TAB, true);
        } else {
            // 代理对（非 BMP 字符）连着发送，让 Windows 自行组合
            append_unicode_event(inputs, character, false);
            append_unicode_event(inputs, character, true);
        }

        if (interval_ms > 0) {
            flush_inputs(inputs);
            sleep_ms(interval_ms);
        } else if (inputs.size() >= 64) {
            flush_inputs(inputs);
        }
    }
    flush_inputs(inputs);
}

void send_hotkey(const std::vector<std::string>& names) {
    std::vector<WORD> modifiers;
    std::vector<WORD> keys;

    for (const std::string& name : names) {
        std::vector<WORD> extra_modifiers;
        const WORD vk = resolve_key(name, extra_modifiers);
        for (const WORD modifier : extra_modifiers) {
            if (std::find(modifiers.begin(), modifiers.end(), modifier) == modifiers.end()) {
                modifiers.push_back(modifier);
            }
        }
        if (is_modifier(vk)) {
            if (std::find(modifiers.begin(), modifiers.end(), vk) == modifiers.end()) {
                modifiers.push_back(vk);
            }
        } else {
            keys.push_back(vk);
        }
    }

    if (keys.empty()) {
        throw std::runtime_error("'keys' must contain at least one non-modifier key");
    }

    std::vector<INPUT> inputs;
    inputs.reserve(modifiers.size() * 2 + keys.size() * 2);
    for (const WORD modifier : modifiers) append_key_event(inputs, modifier, false);
    for (const WORD key : keys) append_key_event(inputs, key, false);
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) append_key_event(inputs, *it, true);
    for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) append_key_event(inputs, *it, true);
    flush_inputs(inputs);
}

nlohmann::json keyboard_handler(const nlohmann::json& args) {
    const std::string action = util::to_lower(util::trim(jargs::get_string(args, "action", "")));
    if (action.empty()) {
        throw std::runtime_error("'action' is required");
    }

    int interval_ms = jargs::get_int(args, "interval_ms", 0);
    if (interval_ms < 0) interval_ms = 0;

    if (action == "type") {
        const std::string text = jargs::get_string(args, "text", "");
        type_text(text, interval_ms);
        return text_content("typed " + std::to_string(util::to_wide(text).size()) + " character(s)");
    }

    if (action == "key") {
        const std::string name = jargs::get_string(args, "key", "");
        if (name.empty()) throw std::runtime_error("'key' is required for action 'key'");
        std::vector<WORD> extra_modifiers;
        const WORD vk = resolve_key(name, extra_modifiers);

        const int repeat = (std::max)(1, jargs::get_int(args, "repeat", 1));
        for (int i = 0; i < repeat; ++i) {
            if (!extra_modifiers.empty()) {
                // 符号键本身需要 Shift/Ctrl/Alt，走组合键路径发送
                std::vector<INPUT> inputs;
                for (const WORD modifier : extra_modifiers) append_key_event(inputs, modifier, false);
                append_key_event(inputs, vk, false);
                append_key_event(inputs, vk, true);
                for (auto it = extra_modifiers.rbegin(); it != extra_modifiers.rend(); ++it) {
                    append_key_event(inputs, *it, true);
                }
                flush_inputs(inputs);
            } else {
                tap_key(vk);
            }
            if (i + 1 < repeat) sleep_ms(interval_ms > 0 ? interval_ms : 20);
        }
        return text_content("pressed '" + name + "' x" + std::to_string(repeat));
    }

    if (action == "hotkey") {
        auto it = args.find("keys");
        if (it == args.end() || !it->is_array() || it->empty()) {
            throw std::runtime_error("'keys' must be a non-empty array of key names");
        }
        std::vector<std::string> names;
        for (const nlohmann::json& item : *it) {
            if (!item.is_string()) throw std::runtime_error("every entry of 'keys' must be a string");
            names.push_back(item.get<std::string>());
        }
        send_hotkey(names);

        std::string joined;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i > 0) joined += "+";
            joined += names[i];
        }
        return text_content("sent hotkey " + joined);
    }

    if (action == "key_down" || action == "key_up") {
        const std::string name = jargs::get_string(args, "key", "");
        if (name.empty()) throw std::runtime_error(std::string("'key' is required for action '") + action + "'");
        std::vector<WORD> extra_modifiers;
        const WORD vk = resolve_key(name, extra_modifiers);

        std::vector<INPUT> inputs;
        const bool key_up = (action == "key_up");
        if (!key_up) {
            for (const WORD modifier : extra_modifiers) append_key_event(inputs, modifier, false);
        }
        append_key_event(inputs, vk, key_up);
        if (key_up) {
            for (auto it = extra_modifiers.rbegin(); it != extra_modifiers.rend(); ++it) {
                append_key_event(inputs, *it, true);
            }
        }
        flush_inputs(inputs);
        return text_content(std::string("key '") + name + "' " + (key_up ? "released" : "pressed"));
    }

    throw std::runtime_error("unknown keyboard action: '" + action + "'");
}

}  // namespace

Tool make_keyboard_tool() {
    const nlohmann::json properties = {
        {"action",
         {{"type", "string"},
          {"enum", {"type", "key", "hotkey", "key_down", "key_up"}},
          {"description",
           "type: send the string in 'text'. key: press and release 'key'. hotkey: press 'keys' together. "
           "key_down / key_up: hold or release 'key'."}}},
        {"text", {{"type", "string"}, {"description", "Text to type, for action 'type'. Supports Unicode."}}},
        {"key",
         {{"type", "string"},
          {"description",
           "Key name for key / key_down / key_up. Examples: a, 7, f5, enter, esc, tab, space, backspace, delete, "
           "home, end, pageup, pagedown, up, down, left, right, ctrl, shift, alt, win."}}},
        {"keys",
         {{"type", "array"},
          {"items", {{"type", "string"}}},
          {"description", "Key names pressed together, for action 'hotkey'. Example: [\"ctrl\",\"c\"]."}}},
        {"repeat", {{"type", "integer"}, {"description", "How many times to press the key. Default 1."}}},
        {"interval_ms",
         {{"type", "integer"},
          {"description", "Delay between repeated keys or typed characters, in milliseconds. Default 0."}}},
    };

    Tool tool;
    tool.name = "keyboard";
    tool.description =
        "Control the Windows keyboard. 'type' sends arbitrary Unicode text using scancode-based input, so it "
        "works regardless of the active keyboard layout. 'key' presses a single named key, 'hotkey' presses a "
        "combination such as [\"ctrl\",\"shift\",\"s\"], and 'key_down' / 'key_up' hold or release a key. "
        "Input is injected with SendInput, so it cannot reach windows that run at a higher integrity level than "
        "this process.";
    tool.input_schema = {{"type", "object"}, {"properties", properties}, {"required", {"action"}}};
    tool.handler = keyboard_handler;
    return tool;
}
