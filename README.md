# mkv-win-mcp

Windows 上的 MCP 服务，让 AI 能看见屏幕、操作鼠标键盘。C++17 + CMake 编写，stdio 传输，不占网络端口。

模型典型的使用流程：截图看画面 → 点一下 / 敲几个键 → 再截图确认。

## 快速开始

编译（需要 VS2022 和 CMake ≥ 3.20）。默认用 Ninja：

```powershell
$env:CMAKE_GENERATOR = 'Ninja'
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Ninja 比 VS 生成器快不少，代价是**必须在 VS 开发者命令行里执行**——Ninja 和 `cl.exe` 都得在 PATH 上，而 VS 生成器能自己找到工具链。开始菜单里搜 "x64 Native Tools Command Prompt for VS 2022" 打开即可。Ninja 本身就在 VS 安装目录里（`Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\`），不用另外装。

两点注意：

- `build/` 如果是之前用 VS 生成器配置的，切 Ninja 前得先删掉（`Remove-Item -Recurse build`），否则 CMake 会报生成器不一致。
- 单配置生成器没有 `--config`，配置类型靠 `-DCMAKE_BUILD_TYPE` 指定。

不想用 Ninja 就照旧走 VS 生成器：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

装进客户端（自动识别 Trae / WorkBuddy / OpenCode）：

```powershell
.\install.ps1
```

两种生成器的产物都落在 `build/bin/Release/mkv_win_mcp.exe`，静态链接，不依赖额外 DLL。

## 两个工具

### screenshot — 看屏幕

抓屏幕（默认整块虚拟桌面，也可指定显示器或某个矩形），等比缩放到 1000 像素以内，默认以 base64 文本返回 PNG 或 JPEG。

- `monitor`：显示器序号，`0` 是主屏，`-1`（默认）是全部显示器拼起来的桌面
- `region`：只抓一块，给 `x` `y` `width` `height`
- `max_width` / `max_height`：默认都是 1000
- `format`：`png`（默认，无损）或 `jpeg`（小）；`quality` 默认 80
- `output`：`base64`（默认，返回 base64 文本）/ `file`（存盘并返回路径）；选 `file` 时用 `save_path` 指定位置

`save_path` 原样交给系统，相对路径按服务进程的工作目录解析，父目录需要已存在；写出的格式跟随 `format`。

返回内容里除了 base64 文本（或落盘路径），总会带上一行文字和一个 JSON 块，同时给出**原始尺寸**和**缩放后尺寸**，方便把图片上的像素坐标换算回屏幕坐标：

```text
captured 400x300 (original) at (0,0), returned 200x150 (scaled) png at 0.5x, 20908 bytes
{"original":{"width":400,"height":300},"scaled":{"width":200,"height":150},"scale":0.5,"rect":{"x":0,"y":0,"width":400,"height":300},"format":"png","bytes":20908}
```

`output` 为 `file` 时，JSON 块里还会多一个 `path`。两种输出模式都会带上这些信息。

限制尺寸是有原因的：4K 原图 base64 之后是好几 MB 的文本，很容易把上下文撑爆。

### mouse_key — 鼠标键盘动作序列

`steps` 是一个动作数组，按顺序执行。每步的 `action` 带设备前缀，鼠标动作用 `mouse.`，键盘用 `key.`：

- 鼠标：`mouse.move`、`mouse.move_relative`、`mouse.click`、`mouse.double_click`、`mouse.down`、`mouse.up`、`mouse.drag`、`mouse.scroll`、`mouse.position`
- 键盘：`key.type`、`key.press`、`key.hotkey`、`key.down`、`key.up`

每步可选 `delay_ms`，执行完这步后停顿再走下一步。

坐标是虚拟屏幕的绝对像素，和截图里的位置一一对应。按钮默认 `left`，另有 `right` `middle` `x1` `x2`。`mouse.click` 可以顺便带上 `x` `y`，`mouse.drag` 用 `to_x` `to_y` 指定终点，`mouse.scroll` 的 `dy` 为正表示向上滚。

拖拽还支持拐弯：给一串 `waypoints`（`[{"x":..,"y":..}, ...]`），按下后依次经过这些点，最后在 `to_x` `to_y` 松手（不给 `to_x` `to_y` 就在最后一个途经点松手）。给 `duration_ms` 时它表示整段路径的总时长，按各段距离分配。

`mouse.move` 和 `mouse.drag` 默认按距离自动定时长，沿最小急动度曲线带一点弧度和手抖地滑过去，节奏接近人手而不是匀速直线；落点始终精确。实际上**所有会移动光标的动作都是这样**——`mouse.click`、`mouse.double_click`、`mouse.down`、`mouse.up`、`mouse.scroll` 带上 `x` `y` 时，以及 `mouse.move_relative`，都会先滑过去再动作。想让光标瞬间跳过去就显式传 `duration_ms: 0`，想固定耗时就直接传毫秒数。

```json
{"steps": [
  {"action": "mouse.move", "x": 640, "y": 400},
  {"action": "mouse.click"},
  {"action": "key.type", "text": "hello world"},
  {"action": "key.press", "key": "enter"}
]}
```

合成一个工具的价值在于跨设备动作可以精确交错。比如按住 Shift 连点两处再松开，按住和松开之间的鼠标动作由同一次调用保证顺序：

```json
{"steps": [
  {"action": "key.down", "key": "shift"},
  {"action": "mouse.click", "x": 300, "y": 220},
  {"action": "mouse.click", "x": 520, "y": 340},
  {"action": "key.up", "key": "shift"}
]}
```

`key.type` 按 Unicode 注入，跟当前键盘布局无关，中文可以正常输入。键名支持 `a`–`z`、`0`–`9`、`f1`–`f24`，以及 `enter` `esc` `tab` `space` `up` `delete` `ctrl` `shift` `alt` `win` 等。

某一步失败会立刻中止，错误信息里带上出错步的序号和动作名；如果这次调用按下了还没松开的按钮或键，会先松开再报错，不会把它们留在按下状态。

两个工具都还有几个次要参数（重复次数、平滑移动耗时、点击间隔等），在客户端里执行一次 `tools/list` 就能看到完整定义。

## 手动配置

```json
{
  "mcpServers": {
    "mkv-win": {
      "command": "d:/001M/workspace/ait/mkv_win_mcp/build/bin/Release/mkv_win_mcp.exe",
      "args": []
    }
  }
}
```

OpenCode 的写法不一样，放在 `mcp` 下：

```json
{
  "mcp": {
    "mkv-win": {
      "type": "local",
      "command": ["d:/001M/workspace/ait/mkv_win_mcp/build/bin/Release/mkv_win_mcp.exe"],
      "enabled": true
    }
  }
}
```

## 注意事项

- **点不动管理员窗口**：Windows 的 UIPI 限制，除非本进程也以管理员身份运行。
- **锁屏时不能用**，抓屏和输入注入都会失败。
- **截图里看不到鼠标光标**，GDI 抓屏不画光标。
- 硬件覆盖层的视频、独占全屏的游戏可能抓不到。

出错时工具会返回 `isError: true` 和具体原因，不会静默失败。
