#include <nlohmann/json.hpp>

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "mouse.h"
#include "util.h"

namespace {

struct ButtonFlags {
    DWORD down;
    DWORD up;
    DWORD data;
};

ButtonFlags button_flags(const std::string& name) {
    if (name == "left") return {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0};
    if (name == "right") return {MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0};
    if (name == "middle") return {MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0};
    if (name == "x1" || name == "back") return {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1};
    if (name == "x2" || name == "forward") return {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2};
    throw std::runtime_error("unknown mouse button: '" + name + "'");
}

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

// 屏幕像素坐标 -> SendInput 归一化虚拟桌面坐标
// Windows 把 0..65535 均分成 width 个像素块，逆映射是 pixel = value * width / 65536。
// 因此必须瞄准目标像素块的中点：取像素块起点会落到上一块（实测 x=1/2 会偏 -1），
// 取末尾值又会溢出到下一块。
void to_absolute(int x, int y, LONG& out_x, LONG& out_y) {
    const int origin_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int origin_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 1 || height <= 1) {
        throw std::runtime_error("cannot determine virtual screen metrics");
    }

    const double fx =
        (static_cast<double>(x - origin_x) * 65536.0 + 32768.0) / static_cast<double>(width);
    const double fy =
        (static_cast<double>(y - origin_y) * 65536.0 + 32768.0) / static_cast<double>(height);
    out_x = static_cast<LONG>(std::lround((std::max)(0.0, (std::min)(65535.0, fx))));
    out_y = static_cast<LONG>(std::lround((std::max)(0.0, (std::min)(65535.0, fy))));
}

void move_to_point(int x, int y) {
    std::vector<INPUT> inputs(1);
    INPUT& input = inputs.front();
    input.type = INPUT_MOUSE;
    to_absolute(x, y, input.mi.dx, input.mi.dy);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    flush_inputs(inputs);
}

// 系统时钟粒度默认 ~15.5ms，会把 sleep 拉长成 16/31 这种台阶；
// 移动期间临时提到 1ms，结束后立刻还回去。
class TimerResolutionGuard {
public:
    TimerResolutionGuard() : active_(timeBeginPeriod(1) == TIMERR_NOERROR) {}
    ~TimerResolutionGuard() {
        if (active_) timeEndPeriod(1);
    }

    TimerResolutionGuard(const TimerResolutionGuard&) = delete;
    TimerResolutionGuard& operator=(const TimerResolutionGuard&) = delete;

private:
    bool active_;
};

std::mt19937& rng() {
    static std::mt19937 engine{std::random_device{}()};
    return engine;
}

double random_between(double low, double high) {
    return std::uniform_real_distribution<double>(low, high)(rng());
}

// 最小急动度剖面 s(t) = 10t^3 - 15t^4 + 6t^5：s(0)=0、s(1)=1、两端速度为 0，
// 位移速度呈慢-快-慢的钟形，接近人手的一次挥动。
double min_jerk(double t) {
    return t * t * t * (10.0 + t * (-15.0 + 6.0 * t));
}

// 距离越远给的时间越长，但按指数饱和，跨屏也不会慢得离谱。
int human_duration_ms(double distance) {
    constexpr double kBase = 120.0;  // 起手耗时
    constexpr double kSpan = 530.0;  // 长距离额外增加的上限
    constexpr double kTau = 500.0;   // 饱和尺度（像素）
    if (distance < 4.0) return 0;    // 几乎原地，不必画轨迹
    return static_cast<int>(std::lround(kBase + kSpan * (1.0 - std::exp(-distance / kTau))));
}

// 拟人移动：最小急动度剖面 + 垂直于运动方向的轻微弧度 + 手抖 + 随机步间隔。
// duration_ms < 0 表示按距离自动定时长；== 0 表示瞬时；> 0 表示指定总时长。
// 最后一步无条件写到精确目标，终点不受弧度与抖动影响。
void move_humanized(int x, int y, int duration_ms) {
    POINT start{};
    if (!GetCursorPos(&start)) {
        move_to_point(x, y);
        return;
    }

    const double dx = static_cast<double>(x - start.x);
    const double dy = static_cast<double>(y - start.y);
    const double distance = std::hypot(dx, dy);

    int duration = duration_ms;
    if (duration < 0) duration = human_duration_ms(distance);
    if (duration <= 0 || distance < 1.0) {
        move_to_point(x, y);
        return;
    }

    const int steps = (std::max)(10, (std::min)(55, duration / 12));

    // 弧度和手抖都垂直于运动方向，用 sin(pi*t) 淡入淡出，保证起止点干净。
    const double perp_x = (distance > 0.0) ? (-dy / distance) : 0.0;
    const double perp_y = (distance > 0.0) ? (dx / distance) : 0.0;
    const double bow = random_between(0.02, 0.06) * distance * (random_between(0.0, 1.0) < 0.5 ? -1.0 : 1.0);
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kJitter = 1.5;  // 手抖幅度（像素）

    TimerResolutionGuard timer;

    const auto began = std::chrono::steady_clock::now();
    const double step_ms = static_cast<double>(duration) / (steps - 1);
    double due = 0.0;
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i - 1) / (steps - 1);
        const double envelope = std::sin(kPi * t);
        const double offset = bow * envelope + random_between(-kJitter, kJitter) * envelope;
        const double s = min_jerk(t);
        const double px = (i == steps) ? x : start.x + dx * s + perp_x * offset;
        const double py = (i == steps) ? y : start.y + dy * s + perp_y * offset;
        move_to_point(static_cast<int>(std::lround(px)), static_cast<int>(std::lround(py)));

        if (i == steps) break;
        // 步间隔乘随机因子，避免等距节拍；按绝对时刻排程，sleep 偏长也能自我纠正
        due += step_ms * random_between(0.85, 1.15);
        const double elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        sleep_ms(static_cast<int>(std::lround((std::max)(0.0, due - elapsed))));
    }
}

// 依次走完 stops 的每个点，调用方需保证按钮处于按下状态。
// total_duration_ms < 0 时每段按距离自动定时长；>= 0 时视为整段总时长，
// 按各段距离比例分配，余量补给最后一段，使总耗时与请求一致。
void move_through_stops(const std::vector<POINT>& stops, int total_duration_ms) {
    std::vector<double> lengths(stops.size(), 0.0);
    double total = 0.0;
    POINT previous{};
    if (GetCursorPos(&previous)) {
        for (std::size_t i = 0; i < stops.size(); ++i) {
            lengths[i] = std::hypot(static_cast<double>(stops[i].x - previous.x),
                                    static_cast<double>(stops[i].y - previous.y));
            total += lengths[i];
            previous = stops[i];
        }
    }

    const int count = static_cast<int>(stops.size());
    int remaining = total_duration_ms;
    for (int i = 0; i < count; ++i) {
        int segment = -1;
        if (total_duration_ms >= 0) {
            if (i + 1 == count) {
                segment = remaining;
            } else if (total > 0.0) {
                // 截断而非四舍五入，保证各段之和不超过总时长
                segment = static_cast<int>(total_duration_ms * (lengths[i] / total));
            } else {
                segment = total_duration_ms / count;
            }
            remaining -= segment;
        }
        move_humanized(stops[i].x, stops[i].y, segment);
    }
}

// 双击的两次点击间隔：必须落在系统双击时间（默认 500ms）内，
// 系统才会把第二次按下合成成 WM_*BUTTONDBLCLK，否则只当成两次单击。
constexpr int kDoubleClickGapMs = 30;

void press_button(const ButtonFlags& flags) {
    std::vector<INPUT> inputs(1);
    INPUT& input = inputs.front();
    input.type = INPUT_MOUSE;
    input.mi.mouseData = flags.data;
    input.mi.dwFlags = flags.down;
    flush_inputs(inputs);
}

void release_button(const ButtonFlags& flags) {
    std::vector<INPUT> inputs(1);
    INPUT& input = inputs.front();
    input.type = INPUT_MOUSE;
    input.mi.mouseData = flags.data;
    input.mi.dwFlags = flags.up;
    flush_inputs(inputs);
}

void scroll_wheel(int delta, bool horizontal) {
    if (delta == 0) return;
    const int limit = 32767;
    const int clamped = (std::max)(-limit, (std::min)(limit, delta));

    std::vector<INPUT> inputs(1);
    INPUT& input = inputs.front();
    input.type = INPUT_MOUSE;
    input.mi.mouseData = static_cast<DWORD>(clamped);
    input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    flush_inputs(inputs);
}

std::string cursor_position_text() {
    POINT point{};
    if (!GetCursorPos(&point)) {
        throw std::runtime_error("GetCursorPos failed: " + util::last_error_string(GetLastError()));
    }
    return "(" + std::to_string(point.x) + "," + std::to_string(point.y) + ")";
}

int require_int(const nlohmann::json& args, const char* key) {
    if (!jargs::has(args, key)) {
        throw std::runtime_error(std::string("argument '") + key + "' is required for this action");
    }
    return jargs::get_int(args, key, 0);
}

nlohmann::json mouse_handler(const nlohmann::json& args) {
    const std::string action = util::to_lower(util::trim(jargs::get_string(args, "action", "")));
    if (action.empty()) {
        throw std::runtime_error("'action' is required");
    }

    const std::string button = util::to_lower(jargs::get_string(args, "button", "left"));

    // 所有会移动光标的动作都走拟人轨迹：<0 按距离自动定时长，0 瞬移，>0 固定耗时
    const int duration = jargs::has(args, "duration_ms") ? jargs::get_int(args, "duration_ms", -1) : -1;

    if (action == "position") {
        return text_content("cursor is at " + cursor_position_text());
    }

    if (action == "move") {
        const int x = require_int(args, "x");
        const int y = require_int(args, "y");
        move_humanized(x, y, duration);
        return text_content("moved cursor to (" + std::to_string(x) + "," + std::to_string(y) + ")");
    }

    if (action == "move_relative") {
        const int dx = jargs::get_int(args, "dx", 0);
        const int dy = jargs::get_int(args, "dy", 0);
        if (dx == 0 && dy == 0) {
            throw std::runtime_error("move_relative requires a non-zero 'dx' or 'dy'");
        }
        POINT origin{};
        if (!GetCursorPos(&origin)) {
            throw std::runtime_error("GetCursorPos failed: " + util::last_error_string(GetLastError()));
        }
        move_humanized(origin.x + dx, origin.y + dy, duration);
        return text_content("moved cursor by (" + std::to_string(dx) + "," + std::to_string(dy) + ")");
    }

    if (action == "click" || action == "double_click") {
        const bool doubled = (action == "double_click");
        if (jargs::has(args, "x") && jargs::has(args, "y")) {
            move_humanized(jargs::get_int(args, "x", 0), jargs::get_int(args, "y", 0), duration);
        }
        const ButtonFlags flags = button_flags(button);
        press_button(flags);
        release_button(flags);
        if (doubled) {
            sleep_ms(kDoubleClickGapMs);
            press_button(flags);
            release_button(flags);
        }
        return text_content(button + (doubled ? " double_click" : " click") + " at " +
                            cursor_position_text());
    }

    if (action == "down" || action == "up") {
        if (jargs::has(args, "x") && jargs::has(args, "y")) {
            move_humanized(jargs::get_int(args, "x", 0), jargs::get_int(args, "y", 0), duration);
        }
        const ButtonFlags flags = button_flags(button);
        if (action == "down") {
            press_button(flags);
        } else {
            release_button(flags);
        }
        return text_content(button + " button " + (action == "down" ? "pressed" : "released") + " at " +
                            cursor_position_text());
    }

    if (action == "drag") {
        int from_x = 0;
        int from_y = 0;
        const bool has_start = jargs::has(args, "x") && jargs::has(args, "y");
        if (has_start) {
            from_x = jargs::get_int(args, "x", 0);
            from_y = jargs::get_int(args, "y", 0);
        }

        // 依次经过的点：waypoints 数组在前，to_x/to_y 若给出则作为最后一点
        std::vector<POINT> stops;
        const nlohmann::json::const_iterator waypoints = args.find("waypoints");
        if (waypoints != args.end() && !waypoints->is_null()) {
            if (!waypoints->is_array()) {
                throw std::runtime_error("argument 'waypoints' must be an array of {x,y} objects");
            }
            for (const nlohmann::json& item : *waypoints) {
                if (!item.is_object()) {
                    throw std::runtime_error("every 'waypoints' entry must be an {x,y} object");
                }
                stops.push_back({require_int(item, "x"), require_int(item, "y")});
            }
        }
        if (jargs::has(args, "to_x") || jargs::has(args, "to_y")) {
            stops.push_back({require_int(args, "to_x"), require_int(args, "to_y")});
        }
        if (stops.empty()) {
            throw std::runtime_error("drag requires 'to_x'/'to_y' or a non-empty 'waypoints'");
        }

        const ButtonFlags flags = button_flags(button);
        // 起点只是定位，不计入整段拖拽的总时长；请求瞬移时同样瞬移
        if (has_start) move_humanized(from_x, from_y, duration == 0 ? 0 : -1);
        press_button(flags);
        try {
            move_through_stops(stops, duration);
        } catch (...) {
            release_button(flags);
            throw;
        }
        release_button(flags);

        const POINT& last = stops.back();
        std::string summary = button + " drag to (" + std::to_string(last.x) + "," + std::to_string(last.y) + ")";
        if (stops.size() > 1) summary += " via " + std::to_string(stops.size() - 1) + " waypoint(s)";
        return text_content(summary);
    }

    if (action == "scroll") {
        if (jargs::has(args, "x") && jargs::has(args, "y")) {
            move_humanized(jargs::get_int(args, "x", 0), jargs::get_int(args, "y", 0), duration);
        }
        const int dx = jargs::get_int(args, "dx", 0);
        const int dy = jargs::get_int(args, "dy", 0);
        if (dx == 0 && dy == 0) {
            throw std::runtime_error("scroll requires a non-zero 'dx' or 'dy'");
        }
        scroll_wheel(dy * WHEEL_DELTA, false);
        scroll_wheel(dx * WHEEL_DELTA, true);
        return text_content("scrolled by (" + std::to_string(dx) + "," + std::to_string(dy) + ") notch(es)");
    }

    throw std::runtime_error("unknown mouse action: '" + action + "'");
}

}  // namespace

Tool make_mouse_tool() {
    const nlohmann::json properties = {
        {"action",
         {{"type", "string"},
          {"enum", {"move", "move_relative", "click", "double_click", "down", "up", "drag", "scroll", "position"}},
          {"description",
           "move: absolute move to x,y. move_relative: move by dx,dy. click: one press and release, optionally after "
           "moving to x,y. double_click: two presses and releases in quick succession, so the system sees a real "
           "double click. down / up: press or release a button and leave it in that state. drag: hold a button and "
           "move to to_x,to_y, optionally passing through the 'waypoints' first. scroll: turn the wheel by dx,dy "
           "notches (positive dy scrolls up). position: report "
           "the cursor position."}}},
        {"x", {{"type", "integer"}, {"description", "Absolute X in virtual-screen pixels."}}},
        {"y", {{"type", "integer"}, {"description", "Absolute Y in virtual-screen pixels."}}},
        {"dx", {{"type", "integer"}, {"description", "Relative X offset (move_relative) or wheel notches (scroll)."}}},
        {"dy", {{"type", "integer"}, {"description", "Relative Y offset (move_relative) or wheel notches (scroll)."}}},
        {"to_x", {{"type", "integer"}, {"description", "Drag target X. Optional when waypoints is given."}}},
        {"to_y", {{"type", "integer"}, {"description", "Drag target Y. Optional when waypoints is given."}}},
        {"waypoints",
         {{"type", "array"},
          {"description",
           "drag only: a list of intermediate {x,y} points to pass through, in order, while the button stays down. "
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
          {"description", "Mouse button, default left."}}},
        {"duration_ms",
         {{"type", "integer"},
          {"description",
           "Time spent moving the cursor, in milliseconds, for any action that repositions it (move, move_relative, "
           "click, double_click, down, up, drag, scroll). Omit it to derive a human-like duration from the distance "
           "(about 120 ms nearby, up to 650 ms across the screen); 0 jumps straight to the target. For a drag through "
           "waypoints it is the total time for the whole path, split across segments by distance. The final position "
           "is exact either way."}}},
    };

    Tool tool;
    tool.name = "mouse";
    tool.description =
        "Control the Windows mouse cursor. Coordinates are absolute virtual-screen pixels with the origin at "
        "the top-left, matching the image returned by 'screenshot'. Use 'position' to read the current cursor "
        "location. Every action that repositions the cursor glides there along a human-like path by default; pass "
        "duration_ms to control the time, or 0 to teleport. Input is injected with SendInput, so it cannot reach "
        "windows that run at a higher integrity level than this process.";
    tool.input_schema = {{"type", "object"}, {"properties", properties}, {"required", {"action"}}};
    tool.handler = mouse_handler;
    return tool;
}
