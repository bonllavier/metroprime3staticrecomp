#include "screenshot.h"

#include "prime_port_version.h"
#include "runtime_config.h"

#include <aurora/aurora.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <gdiplus.h>
#endif

namespace fs = std::filesystem;

namespace Screenshot {
namespace {

std::atomic<bool> g_busy{false};
std::mutex g_statusMutex;
std::string g_status;
std::chrono::steady_clock::time_point g_statusTime{};

void SetStatus(std::string text) {
    std::lock_guard lock(g_statusMutex);
    g_status = std::move(text);
    g_statusTime = std::chrono::steady_clock::now();
}

std::string Utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string Stamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    char text[32];
    std::snprintf(text, sizeof(text), "%04d%02d%02d_%02d%02d%02d_%03d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, static_cast<int>(ms));
    return text;
}

bool WaitForFile(const fs::path& path, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::uintmax_t lastSize = 0;
    int stable = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (!ec && size > 0) {
            if (size == lastSize && ++stable >= 3) return true;
            lastSize = size;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

#if defined(_WIN32)
// JPEG at quality 92: about a tenth of a PNG of the same frame, without visible loss.
constexpr ULONG kJpegQuality = 92;

bool BmpToJpeg(const fs::path& bmp, const fs::path& jpg) {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    {
        Gdiplus::Bitmap image(bmp.c_str());
        if (image.GetLastStatus() == Gdiplus::Ok) {
            const CLSID jpegEncoder = {0x557CF401, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
            const GUID encoderQuality = {0x1d5be4b5, 0xfa4a, 0x452d, {0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb}};
            ULONG quality = kJpegQuality;
            Gdiplus::EncoderParameters params{};
            params.Count = 1;
            params.Parameter[0].Guid = encoderQuality;
            params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            params.Parameter[0].NumberOfValues = 1;
            params.Parameter[0].Value = &quality;
            ok = image.Save(jpg.c_str(), &jpegEncoder, &params) == Gdiplus::Ok;
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return ok;
}
#endif

} // namespace

std::string BuildId() {
    static const std::string id = [] {
        uint32_t stamp = 0, imageSize = 0;
#if defined(_WIN32)
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        if (base != nullptr) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            stamp = nt->FileHeader.TimeDateStamp;
            imageSize = nt->OptionalHeader.SizeOfImage;
        }
#endif
        // FNV-1a over the link time, image size and version: stable for one executable.
        uint32_t hash = 2166136261u;
        const auto mix = [&](const void* data, size_t size) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
        };
        mix(&stamp, sizeof(stamp));
        mix(&imageSize, sizeof(imageSize));
        mix(PRIME_PORT_VERSION, sizeof(PRIME_PORT_VERSION) - 1);
        char text[16];
        std::snprintf(text, sizeof(text), "%08x", hash);
        return std::string(text);
    }();
    return id;
}

fs::path Directory() { return RuntimeConfigFile::ApplicationDataDirectory() / "Screenshots"; }

bool Take() {
    if (g_busy.exchange(true)) return false;
    const fs::path folder = Directory();
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        SetStatus("Could not create " + Utf8(folder));
        g_busy.store(false);
        return false;
    }
    const std::string name = std::string("Corruption3PC_v") + PRIME_PORT_VERSION + "_" + BuildId() + "_" + Stamp();
    const fs::path bmp = folder / (name + ".bmp");
    const fs::path jpg = folder / (name + ".jpg");
    aurora_request_screenshot(Utf8(bmp).c_str());
    std::thread([bmp, jpg] {
        std::error_code error;
        bool saved = false;
        if (WaitForFile(bmp, std::chrono::milliseconds(4000))) {
#if defined(_WIN32)
            if (BmpToJpeg(bmp, jpg)) {
                fs::remove(bmp, error);
                saved = true;
            }
#endif
        }
        if (saved) SetStatus("Screenshot saved: Screenshots/" + Utf8(jpg.filename()));
        else if (fs::exists(bmp, error)) SetStatus("Screenshot saved (BMP): Screenshots/" + Utf8(bmp.filename()));
        else SetStatus("Screenshot failed");
        g_busy.store(false);
    }).detach();
    return true;
}

std::string Status() {
    std::lock_guard lock(g_statusMutex);
    return g_status;
}

double SecondsSinceStatus() {
    std::lock_guard lock(g_statusMutex);
    if (g_statusTime.time_since_epoch().count() == 0) return 1e9;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_statusTime).count();
}

} // namespace Screenshot
