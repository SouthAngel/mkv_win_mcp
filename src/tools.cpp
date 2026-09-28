#include "tools.h"

#include "keyboard.h"
#include "mouse.h"
#include "screenshot.h"

const std::vector<Tool>& all_tools() {
    static const std::vector<Tool> tools = {
        make_screenshot_tool(),
        make_mouse_tool(),
        make_keyboard_tool(),
    };
    return tools;
}

const Tool* find_tool(const std::string& name) {
    for (const Tool& tool : all_tools()) {
        if (tool.name == name) return &tool;
    }
    return nullptr;
}
