#include <nlohmann/json.hpp>

#include <windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "screenshot.h"
#include "util.h"

namespace {

const int kDefaultMaxSize = 1000;
const int kDefaultJpegQuality = 80;

const wchar_t* const kMimePng = L"image/png";
const wchar_t* const kMimeJpeg = L"image/jpeg";

struct MonitorEntry {
    RECT rect;
    bool primary;
};

struct EnumContext {
    std::vector<MonitorEntry> others;
    MonitorEntry primary{};
    bool has_primary = false;
};

BOOL CALLBACK monitor_proc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* context = reinterpret_cast<EnumContext*>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(MONITORINFOEXW);
    if (GetMonitorInfoW(monitor, &info)) {
        MonitorEntry entry{};
        entry.rect = info.rcMonitor;
        entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        if (entry.primary) {
            context->primary = entry;
            context->has_primary = true;
        } else {
            context->others.push_back(entry);
        }
    }
    return TRUE;
}

// 显示器列表中主屏固定排在第 0 位，其余按系统枚举顺序
std::vector<MonitorEntry> enumerate_monitors() {
    EnumContext context;
    EnumDisplayMonitors(nullptr, nullptr, monitor_proc, reinterpret_cast<LPARAM>(&context));

    std::vector<MonitorEntry> monitors;
    if (context.has_primary) monitors.push_back(context.primary);
    for (const MonitorEntry& entry : context.others) monitors.push_back(entry);
    return monitors;
}

RECT resolve_capture_rect(const nlohmann::json& args) {
    auto region = args.find("region");
    if (region != args.end() && region->is_object()) {
        RECT rect{};
        rect.left = jargs::get_int(*region, "x", 0);
        rect.top = jargs::get_int(*region, "y", 0);
        rect.right = rect.left + jargs::get_int(*region, "width", 0);
        rect.bottom = rect.top + jargs::get_int(*region, "height", 0);
        if (rect.right <= rect.left || rect.bottom <= rect.top) {
            throw std::runtime_error("region requires positive width and height");
        }
        return rect;
    }

    const int monitor = jargs::get_int(args, "monitor", -1);
    if (monitor < 0) {
        RECT rect{};
        rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        rect.right = rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        rect.bottom = rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (rect.right <= rect.left || rect.bottom <= rect.top) {
            throw std::runtime_error("cannot determine virtual screen size");
        }
        return rect;
    }

    const std::vector<MonitorEntry> monitors = enumerate_monitors();
    if (monitor >= static_cast<int>(monitors.size())) {
        throw std::runtime_error("monitor index " + std::to_string(monitor) + " out of range, detected " +
                                 std::to_string(monitors.size()) + " monitor(s)");
    }
    return monitors[static_cast<std::size_t>(monitor)].rect;
}

// 自顶向下的 32bpp DIB，析构时释放 GDI 对象
struct ScreenBitmap {
    HDC screen_dc = nullptr;
    HDC mem_dc = nullptr;
    HBITMAP bitmap = nullptr;
    void* bits = nullptr;
    int width = 0;
    int height = 0;

    ~ScreenBitmap() {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (mem_dc != nullptr) DeleteDC(mem_dc);
        if (screen_dc != nullptr) DeleteDC(screen_dc);
    }

    ScreenBitmap() = default;
    ScreenBitmap(const ScreenBitmap&) = delete;
    ScreenBitmap& operator=(const ScreenBitmap&) = delete;
};

void capture_screen(const RECT& rect, ScreenBitmap& shot) {
    shot.width = rect.right - rect.left;
    shot.height = rect.bottom - rect.top;

    shot.screen_dc = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
    if (shot.screen_dc == nullptr) {
        throw std::runtime_error("CreateDC failed: " + util::last_error_string(GetLastError()));
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = shot.width;
    info.bmiHeader.biHeight = -shot.height;  // 负高度 = 自顶向下
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    shot.mem_dc = CreateCompatibleDC(shot.screen_dc);
    if (shot.mem_dc == nullptr) {
        throw std::runtime_error("CreateCompatibleDC failed: " + util::last_error_string(GetLastError()));
    }

    shot.bitmap = CreateDIBSection(shot.screen_dc, &info, DIB_RGB_COLORS, &shot.bits, nullptr, 0);
    if (shot.bitmap == nullptr || shot.bits == nullptr) {
        throw std::runtime_error("CreateDIBSection failed: " + util::last_error_string(GetLastError()));
    }

    HGDIOBJ previous = SelectObject(shot.mem_dc, shot.bitmap);
    const BOOL ok = BitBlt(shot.mem_dc, 0, 0, shot.width, shot.height, shot.screen_dc, rect.left, rect.top,
                           SRCCOPY | CAPTUREBLT);
    SelectObject(shot.mem_dc, previous);
    if (!ok) {
        throw std::runtime_error("BitBlt failed: " + util::last_error_string(GetLastError()));
    }
}

bool find_encoder(const wchar_t* mime, CLSID& clsid) {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) return false;

    std::vector<unsigned char> buffer(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;

    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

std::vector<unsigned char> encode_image(Gdiplus::Bitmap& bitmap, const std::wstring& mime, int quality) {
    CLSID clsid{};
    if (!find_encoder(mime.c_str(), clsid)) {
        throw std::runtime_error("no image encoder available for the requested format");
    }

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        throw std::runtime_error("CreateStreamOnHGlobal failed: " + util::last_error_string(GetLastError()));
    }

    Gdiplus::EncoderParameters parameters{};
    parameters.Count = 1;
    parameters.Parameter[0].Guid = Gdiplus::EncoderQuality;
    parameters.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    parameters.Parameter[0].NumberOfValues = 1;
    ULONG quality_value = static_cast<ULONG>(quality);
    parameters.Parameter[0].Value = &quality_value;

    const Gdiplus::EncoderParameters* used_parameters = (mime == kMimeJpeg) ? &parameters : nullptr;
    const Gdiplus::Status status = bitmap.Save(stream, &clsid, used_parameters);
    if (status != Gdiplus::Ok) {
        stream->Release();
        throw std::runtime_error("image encoding failed, GDI+ status " + std::to_string(static_cast<int>(status)));
    }

    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    const std::size_t size = static_cast<std::size_t>(stat.cbSize.QuadPart);

    std::vector<unsigned char> data;
    HGLOBAL handle = nullptr;
    if (GetHGlobalFromStream(stream, &handle) == S_OK) {
        void* memory = GlobalLock(handle);
        if (memory != nullptr && size > 0) {
            const auto* first = static_cast<const unsigned char*>(memory);
            data.assign(first, first + size);
        }
        if (memory != nullptr) GlobalUnlock(handle);
    }
    stream->Release();

    if (data.empty()) {
        throw std::runtime_error("encoded image is empty");
    }
    return data;
}

nlohmann::json screenshot_handler(const nlohmann::json& args) {
    const RECT rect = resolve_capture_rect(args);

    const int max_width = jargs::get_int(args, "max_width", kDefaultMaxSize);
    const int max_height = jargs::get_int(args, "max_height", kDefaultMaxSize);
    if (max_width <= 0 || max_height <= 0) {
        throw std::runtime_error("max_width and max_height must be positive");
    }

    const std::string format = util::to_lower(jargs::get_string(args, "format", "png"));
    const bool use_jpeg = (format == "jpeg" || format == "jpg");
    if (!use_jpeg && format != "png") {
        throw std::runtime_error("format must be 'png' or 'jpeg'");
    }

    const int quality = jargs::get_int(args, "quality", kDefaultJpegQuality);
    if (quality < 1 || quality > 100) {
        throw std::runtime_error("quality must be within 1..100");
    }

    // 先校验再抓屏，避免抓完一轮才因为参数写错失败
    const std::string output_mode = util::to_lower(util::trim(jargs::get_string(args, "output", "base64")));
    if (output_mode != "base64" && output_mode != "file") {
        throw std::runtime_error("output must be 'base64' or 'file'");
    }
    const std::string save_path = jargs::get_string(args, "save_path", "");
    if (output_mode == "file" && save_path.empty()) {
        throw std::runtime_error("output 'file' requires a non-empty 'save_path'");
    }

    ScreenBitmap shot;
    capture_screen(rect, shot);

    const int source_width = shot.width;
    const int source_height = shot.height;

    double scale = (std::min)(static_cast<double>(max_width) / source_width,
                              static_cast<double>(max_height) / source_height);
    if (scale > 1.0) scale = 1.0;
    const int output_width = (std::max)(1, static_cast<int>(std::lround(source_width * scale)));
    const int output_height = (std::max)(1, static_cast<int>(std::lround(source_height * scale)));

    // 直接把 DIB 内存交给 GDI+ 使用，避免额外拷贝
    Gdiplus::Bitmap source_bitmap(source_width, source_height, source_width * 4, PixelFormat32bppRGB,
                                  static_cast<BYTE*>(shot.bits));
    if (source_bitmap.GetLastStatus() != Gdiplus::Ok) {
        throw std::runtime_error("failed to wrap the captured bitmap");
    }

    std::unique_ptr<Gdiplus::Bitmap> scaled;
    Gdiplus::Bitmap* output = &source_bitmap;
    if (output_width != source_width || output_height != source_height) {
        scaled = std::make_unique<Gdiplus::Bitmap>(output_width, output_height, PixelFormat32bppRGB);
        if (scaled->GetLastStatus() != Gdiplus::Ok) {
            throw std::runtime_error("failed to allocate the scaled bitmap");
        }

        Gdiplus::Graphics graphics(scaled.get());
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);

        const Gdiplus::Status status =
            graphics.DrawImage(&source_bitmap, Gdiplus::Rect(0, 0, output_width, output_height), 0, 0,
                               source_width, source_height, Gdiplus::UnitPixel);
        if (status != Gdiplus::Ok) {
            throw std::runtime_error("failed to scale the screenshot, GDI+ status " +
                                     std::to_string(static_cast<int>(status)));
        }
        output = scaled.get();
    }

    const std::wstring mime = use_jpeg ? kMimeJpeg : kMimePng;
    const std::vector<unsigned char> bytes = encode_image(*output, mime, quality);

    // 缩放比用实际输出宽度算，和 lround 之后的像素尺寸保持自洽；
    // 模型拿图片坐标反推屏幕坐标时，靠的是 original / scaled 这两组尺寸。
    const double actual_scale =
        (source_width > 0) ? static_cast<double>(output_width) / static_cast<double>(source_width) : 1.0;
    const double reported_scale = std::lround(actual_scale * 10000.0) / 10000.0;

    std::ostringstream scale_text;
    scale_text << reported_scale;

    std::ostringstream summary;
    summary << "captured " << source_width << "x" << source_height << " (original) at (" << rect.left << ","
            << rect.top << "), returned " << output_width << "x" << output_height << " (scaled) "
            << (use_jpeg ? "jpeg" : "png") << " at " << scale_text.str() << "x, " << bytes.size() << " bytes";

    nlohmann::json dimensions = {
        {"original", {{"width", source_width}, {"height", source_height}}},
        {"scaled", {{"width", output_width}, {"height", output_height}}},
        {"scale", reported_scale},
        {"rect",
         {{"x", rect.left}, {"y", rect.top}, {"width", source_width}, {"height", source_height}}},
        {"format", use_jpeg ? "jpeg" : "png"},
        {"bytes", bytes.size()},
    };

    if (output_mode == "file") {
        const std::wstring wide_path = util::to_wide(save_path);
        std::ofstream file(wide_path.c_str(), std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("cannot open '" + save_path + "' for writing");
        }
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) {
            throw std::runtime_error("failed to write '" + save_path + "'");
        }
        summary << ", written to " << save_path;
        dimensions["path"] = save_path;
    }

    const nlohmann::json size_block = {{"type", "text"}, {"text", dimensions.dump()}};
    const nlohmann::json summary_block = {{"type", "text"}, {"text", summary.str()}};
    if (output_mode == "file") {
        return nlohmann::json::array({summary_block, size_block});
    }

    const std::string encoded = util::base64_encode(bytes.data(), bytes.size());
    return nlohmann::json::array({summary_block, size_block, {{"type", "text"}, {"text", encoded}}});
}

}  // namespace

Tool make_screenshot_tool() {
    const nlohmann::json region = {
        {"type", "object"},
        {"description", "Capture only this rectangle, given in virtual-screen pixels."},
        {"properties",
         {{"x", {{"type", "integer"}}},
          {"y", {{"type", "integer"}}},
          {"width", {{"type", "integer"}}},
          {"height", {{"type", "integer"}}}}},
        {"required", {"x", "y", "width", "height"}},
    };

    const nlohmann::json properties = {
        {"monitor",
         {{"type", "integer"},
          {"description",
           "Monitor index, where 0 is the primary display and the remaining monitors follow in system order. "
           "Omit it (or pass -1) to capture the whole virtual desktop."}}},
        {"region", region},
        {"max_width", {{"type", "integer"}, {"description", "Maximum width of the returned image. Default 1000."}}},
        {"max_height", {{"type", "integer"}, {"description", "Maximum height of the returned image. Default 1000."}}},
        {"format",
         {{"type", "string"},
          {"enum", {"png", "jpeg"}},
          {"description", "Image format. png is lossless but larger, jpeg is smaller. Default png."}}},
        {"quality",
         {{"type", "integer"}, {"description", "JPEG quality within 1..100. Default 80, ignored for png."}}},
        {"output",
         {{"type", "string"},
          {"enum", {"base64", "file"}},
          {"description",
           "How the image comes back. base64 (default) returns the encoded bytes as base64 text. file writes the "
           "image to save_path and returns that path."}}},
        {"save_path",
         {{"type", "string"},
          {"description",
           "Where to write the image when output is 'file'. Passed to the OS as-is, so a relative path resolves "
           "against the server's working directory and the parent directory must already exist. The bytes are "
           "written in the format selected by 'format'."}}},
    };

    Tool tool;
    tool.name = "screenshot";
    tool.description =
        "Capture the Windows desktop and return it so you can see the screen. "
        "By default the whole virtual desktop (all monitors) is captured and downscaled so that it fits "
        "within max_width/max_height, and the encoded bytes come back as base64 text. "
        "Set output to 'file' with a save_path to write it to disk instead. "
        "The reply always reports the captured (original) size and the returned (scaled) size, both as text and "
        "as a machine-readable JSON block, so image pixels can be mapped back to screen pixels. "
        "Call this before and after sending mouse or keyboard input.";
    tool.input_schema = {{"type", "object"}, {"properties", properties}};
    tool.handler = screenshot_handler;
    return tool;
}
