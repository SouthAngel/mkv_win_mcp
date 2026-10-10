#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "input.h"
#include "keyboard.h"
#include "mouse.h"
#include "util.h"

namespace {

void sleep_ms(int milliseconds) {
    if (milliseconds > 0) std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// 动作词表：前缀 + 该设备支持的动作名（不含前缀）。schema 的 action 枚举也由它生成，
// 避免词表和文档两处各写一遍后走样。
struct DeviceActions {
    const char* prefix;
    std::vector<std::string> actions;
};

const std::vector<DeviceActions>& vocabulary() {
    static const std::vector<DeviceActions> table = {
        {"mouse",
         {"move", "move_relative", "click", "double_click", "down", "up", "drag", "scroll", "position"}},
        {"key", {"type", "press", "hotkey", "down", "up"}},
    };
    return table;
}

bool is_known_action(const std::string& prefix, const std::string& action) {
    for (const DeviceActions& entry : vocabulary()) {
        if (prefix != entry.prefix) continue;
        return std::find(entry.actions.begin(), entry.actions.end(), action) != entry.actions.end();
    }
    return false;
}

std::string join(const std::vector<std::string>& items, const std::string& separator) {
    std::string joined;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) joined += separator;
        joined += items[i];
    }
    return joined;
}

// 本次调用留下的按下状态。序列中途失败时要把它们松开，
// 否则一个写错的步骤就能把 Shift 或左键永远留在按下状态。
class HeldState {
public:
    // 按下要"先记后执行"：SendInput 可能只注入了一部分就报错，
    // 那时按钮/键其实已经按下，没有记录就没法在收尾时释放。
    void expect_down(const std::string& prefix, const std::string& action, const nlohmann::json& step) {
        if (action != "down") return;
        const std::string name = target_name(prefix, step);
        if (name.empty()) return;
        std::vector<std::string>& held = (prefix == "mouse") ? buttons_ : keys_;
        if (std::find(held.begin(), held.end(), name) == held.end()) held.push_back(name);
    }

    // 抬起要"后记"：只有确实抬起成功才注销记录，失败时仍按"还按着"处理。
    void confirm_up(const std::string& prefix, const std::string& action, const nlohmann::json& step) {
        if (action != "up") return;
        const std::string name = target_name(prefix, step);
        std::vector<std::string>& held = (prefix == "mouse") ? buttons_ : keys_;
        held.erase(std::remove(held.begin(), held.end(), name), held.end());
    }

    bool empty() const { return buttons_.empty() && keys_.empty(); }

    // 逆序释放：后按下的先松开。返回实际释放掉的名称，供错误信息引用。
    std::vector<std::string> release_all() {
        std::vector<std::string> released;
        for (auto it = keys_.rbegin(); it != keys_.rend(); ++it) {
            nlohmann::json args = {{"key", *it}};
            try {
                run_keyboard_step("up", args);
                released.push_back("key '" + *it + "'");
            } catch (...) {
                // 释放失败不再叠加错误，原始失败原因更重要
            }
        }
        for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it) {
            nlohmann::json args = {{"button", *it}};
            try {
                run_mouse_step("up", args);
                released.push_back("button '" + *it + "'");
            } catch (...) {
            }
        }
        buttons_.clear();
        keys_.clear();
        return released;
    }

private:
    static std::string target_name(const std::string& prefix, const nlohmann::json& step) {
        if (prefix == "mouse") return util::to_lower(jargs::get_string(step, "button", "left"));
        return util::to_lower(jargs::get_string(step, "key", ""));
    }

    std::vector<std::string> buttons_;
    std::vector<std::string> keys_;
};

nlohmann::json mouse_key_handler(const nlohmann::json& args) {
    const nlohmann::json::const_iterator steps_it = args.find("steps");
    if (steps_it == args.end() || steps_it->is_null()) {
        throw std::runtime_error("'steps' is required");
    }
    if (!steps_it->is_array()) {
        throw std::runtime_error("'steps' must be an array of action objects");
    }
    const nlohmann::json& steps = *steps_it;
    if (steps.empty()) {
        throw std::runtime_error("'steps' must contain at least one action");
    }

    HeldState held;
    std::vector<std::string> lines;

    for (std::size_t i = 0; i < steps.size(); ++i) {
        const nlohmann::json& step = steps[i];
        std::string label = "step " + std::to_string(i + 1);
        try {
            if (!step.is_object()) {
                throw std::runtime_error("every step must be an object");
            }
            const std::string raw = util::to_lower(util::trim(jargs::get_string(step, "action", "")));
            if (raw.empty()) {
                throw std::runtime_error("'action' is required");
            }
            label += " (" + raw + ")";

            const std::size_t dot = raw.find('.');
            const std::string prefix = (dot == std::string::npos) ? std::string() : raw.substr(0, dot);
            const std::string action = (dot == std::string::npos) ? std::string() : raw.substr(dot + 1);
            if (prefix.empty() || action.empty() || !is_known_action(prefix, action)) {
                throw std::runtime_error("unknown action '" + raw +
                                         "'; expected 'mouse.<action>' or 'key.<action>'");
            }

            held.expect_down(prefix, action, step);
            const std::string note =
                (prefix == "mouse") ? run_mouse_step(action, step) : run_keyboard_step(action, step);
            held.confirm_up(prefix, action, step);

            lines.push_back(std::to_string(i + 1) + ". " + raw + ": " + note);
            sleep_ms(jargs::get_int(step, "delay_ms", 0));
        } catch (const std::exception& error) {
            const std::vector<std::string> released = held.release_all();
            std::string message = label + " failed: " + error.what();
            if (!released.empty()) {
                message += " (released " + join(released, ", ") + " so they are not left held)";
            }
            throw std::runtime_error(message);
        }
    }

    return text_content(join(lines, "\n"));
}

}  // namespace

Tool make_mouse_key_tool() {
    nlohmann::json action_names = nlohmann::json::array();
    for (const DeviceActions& entry : vocabulary()) {
        for (const std::string& action : entry.actions) {
            action_names.push_back(std::string(entry.prefix) + "." + action);
        }
    }

    const nlohmann::json step_properties = {
        {"action",
         {{"type", "string"},
          {"enum", action_names},
          {"description",
           "'mouse.' actions drive the cursor in absolute virtual-screen pixels; 'key.' actions send keys by name. "
           "See each action's parameters below."}}},
        {"delay_ms",
         {{"type", "integer"},
          {"description", "Pause for this many milliseconds after the step. Default 0. Any action."}}},
        {"x",
         {{"type", "integer"},
          {"description",
           "mouse.move, mouse.click, mouse.double_click, mouse.down, mouse.up, mouse.scroll: absolute X in "
           "virtual-screen pixels."}}},
        {"y",
         {{"type", "integer"},
          {"description",
           "mouse.move, mouse.click, mouse.double_click, mouse.down, mouse.up, mouse.scroll: absolute Y in "
           "virtual-screen pixels."}}},
        {"dx",
         {{"type", "integer"},
          {"description",
           "mouse.move_relative: relative X offset. mouse.scroll: wheel notches, positive scrolls right."}}},
        {"dy",
         {{"type", "integer"},
          {"description",
           "mouse.move_relative: relative Y offset. mouse.scroll: wheel notches, positive scrolls up."}}},
        {"to_x",
         {{"type", "integer"},
          {"description", "mouse.drag: target X. Optional when waypoints is given."}}},
        {"to_y",
         {{"type", "integer"},
          {"description", "mouse.drag: target Y. Optional when waypoints is given."}}},
        {"waypoints",
         {{"type", "array"},
          {"description",
           "mouse.drag: intermediate {x,y} points to pass through, in order, while the button stays down. "
           "to_x,to_y is appended as the final stop when given."},
          {"items",
           {{"type", "object"},
            {"properties",
             {{"x", {{"type", "integer"}, {"description", "Absolute X in virtual-screen pixels."}}},
              {"y", {{"type", "integer"}, {"description", "Absolute Y in virtual-screen pixels."}}}}},
            {"required", {"x", "y"}}}}}},
        {"button",
         {{"type", "string"},
          {"enum", {"left", "right", "middle", "x1", "x2"}},
          {"description",
           "mouse.click, mouse.double_click, mouse.down, mouse.up, mouse.drag: which button. Default left."}}},
        {"duration_ms",
         {{"type", "integer"},
          {"description",
           "Time spent moving the cursor, for every mouse action that repositions it. Omit it to derive a "
           "human-like duration from the distance (about 120 ms nearby, up to 650 ms across the screen); 0 jumps "
           "straight to the target. For mouse.drag through waypoints it is the total time for the whole path, "
           "split across segments by distance. The final position is exact either way."}}},
        {"text",
         {{"type", "string"},
          {"description", "key.type: the text to type. Supports Unicode, independent of keyboard layout."}}},
        {"key",
         {{"type", "string"},
          {"description",
           "key.press, key.down, key.up: key name. Examples: a, 7, f5, enter, esc, tab, space, backspace, "
           "delete, home, end, pageup, pagedown, up, down, left, right, ctrl, shift, alt, win."}}},
        {"keys",
         {{"type", "array"},
          {"items", {{"type", "string"}}},
          {"description", "key.hotkey: key names to press together. Example: [\"ctrl\",\"c\"]."}}},
        {"repeat",
         {{"type", "integer"}, {"description", "key.press: how many times to press the key. Default 1."}}},
        {"interval_ms",
         {{"type", "integer"},
          {"description",
           "key.type: delay between typed characters. key.press: delay between repeats. Default 0."}}},
    };

    Tool tool;
    tool.name = "mouse_key";
    tool.description =
        "Drive the Windows mouse and keyboard with a sequence of actions in a single call. Each entry of 'steps' "
        "has an 'action' with a 'mouse.' or 'key.' prefix; steps run in the given order. Because one call carries "
        "the whole sequence, you can hold Shift with key.down, click with mouse.click, then release with key.up "
        "without other input slipping in between. Coordinates are absolute virtual-screen pixels matching the "
        "image returned by 'screenshot'; mouse.position reports the cursor location. Every action that repositions "
        "the cursor glides there along a human-like path by default; pass duration_ms to control the time, or 0 to "
        "teleport. If a step fails the sequence stops there and any button or key this call had left down is "
        "released. Input is injected with SendInput, so it cannot reach windows that run at a higher integrity "
        "level than this process.";
    tool.input_schema = {
        {"type", "object"},
        {"properties",
         {{"steps",
           {{"type", "array"},
            {"description", "The actions to run, in order. Must contain at least one."},
            {"items",
             {{"type", "object"}, {"properties", step_properties}, {"required", {"action"}}}}}}}},
        {"required", {"steps"}},
    };
    tool.handler = mouse_key_handler;
    return tool;
}
