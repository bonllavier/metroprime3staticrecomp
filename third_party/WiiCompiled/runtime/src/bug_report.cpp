#include "bug_report.h"

#include "prime_port_version.h"
#include "nand_path.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "system_bridge.h"

#include <aurora/aurora.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <dxgi.h>
#include <intrin.h>
#include <gdiplus.h>
#endif

namespace fs = std::filesystem;

namespace BugReport {
namespace {

std::atomic_bool g_busy{false};
std::mutex g_statusMutex;
std::string g_status;

void SetStatus(std::string text) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status = std::move(text);
}

std::string Utf8(const fs::path& path) { return RuntimeConfigFile::PathToUtf8(path); }

std::string Timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &local);
    return buffer;
}

// ---------------------------------------------------------------- ZIP writer
// Minimal "stored" ZIP (no compression): the screenshot is already PNG and the
// transcript is small, so this keeps the report dependency-free.
class ZipWriter {
public:
    explicit ZipWriter(const fs::path& path) : out_(path, std::ios::binary) {}
    bool ok() const { return out_.good(); }

    void Add(const std::string& name, const std::vector<uint8_t>& data) {
        Entry entry;
        entry.name = name;
        entry.crc = Crc32(data.data(), data.size());
        entry.size = static_cast<uint32_t>(data.size());
        entry.offset = static_cast<uint32_t>(out_.tellp());
        Write32(0x04034b50); Write16(20); Write16(0x0800); Write16(0); Write16(dosTime_); Write16(dosDate_);
        Write32(entry.crc); Write32(entry.size); Write32(entry.size);
        Write16(static_cast<uint16_t>(name.size())); Write16(0);
        out_.write(name.data(), static_cast<std::streamsize>(name.size()));
        out_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        entries_.push_back(std::move(entry));
    }

    void Finish() {
        const uint32_t centralStart = static_cast<uint32_t>(out_.tellp());
        for (const Entry& e : entries_) {
            Write32(0x02014b50); Write16(20); Write16(20); Write16(0x0800); Write16(0); Write16(dosTime_); Write16(dosDate_);
            Write32(e.crc); Write32(e.size); Write32(e.size);
            Write16(static_cast<uint16_t>(e.name.size())); Write16(0); Write16(0); Write16(0); Write16(0);
            Write32(0); Write32(e.offset);
            out_.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
        }
        const uint32_t centralSize = static_cast<uint32_t>(out_.tellp()) - centralStart;
        Write32(0x06054b50); Write16(0); Write16(0);
        Write16(static_cast<uint16_t>(entries_.size())); Write16(static_cast<uint16_t>(entries_.size()));
        Write32(centralSize); Write32(centralStart); Write16(0);
        out_.flush();
    }

private:
    struct Entry { std::string name; uint32_t crc, size, offset; };

    static uint32_t Crc32(const uint8_t* data, size_t size) {
        static const std::array<uint32_t, 256> table = [] {
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }
    void Write16(uint16_t v) { out_.write(reinterpret_cast<const char*>(&v), 2); }
    void Write32(uint32_t v) { out_.write(reinterpret_cast<const char*>(&v), 4); }

    std::ofstream out_;
    std::vector<Entry> entries_;
    uint16_t dosTime_ = 0, dosDate_ = (static_cast<uint16_t>(2026 - 1980) << 9) | (1 << 5) | 1;
};

std::vector<uint8_t> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool CopyInto(const fs::path& source, const fs::path& directory, const char* nameOverride = nullptr) {
    std::error_code ec;
    if (!fs::is_regular_file(source, ec) || ec) return false;
    const fs::path target = directory / (nameOverride ? fs::path(nameOverride) : source.filename());
    // The transcript is still open for writing by this process: a byte copy
    // through our own read works where copy_file's sharing mode may not.
    const auto bytes = ReadAll(source);
    if (bytes.empty()) return false;
    std::ofstream out(target, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

#if defined(_WIN32)
std::string WindowsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion")); fn && fn(&info) == 0) {
            std::ostringstream text;
            text << "Windows " << info.dwMajorVersion << "." << info.dwMinorVersion << " build " << info.dwBuildNumber;
            return text.str();
        }
    }
    return "Windows (version unavailable)";
}

std::string CpuBrand() {
    int regs[4]{};
    char brand[49]{};
    for (int i = 0; i < 3; ++i) {
        __cpuid(regs, 0x80000002 + i);
        std::memcpy(brand + i * 16, regs, 16);
    }
    std::string text(brand);
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    return text.empty() ? "unknown" : text;
}

std::string MemoryGiB() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!::GlobalMemoryStatusEx(&status)) return "unknown";
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << status.ullTotalPhys / (1024.0 * 1024.0 * 1024.0) << " GiB total, "
         << status.ullAvailPhys / (1024.0 * 1024.0 * 1024.0) << " GiB free";
    return text.str();
}

std::string Narrow(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n - 1, 0)), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string GpuAdapters() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
        return "unknown (DXGI unavailable)";
    std::ostringstream text;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            if (text.tellp() > 0) text << "; ";
            text << Narrow(desc.Description) << " (" << desc.DedicatedVideoMemory / (1024 * 1024) << " MB VRAM)";
        }
        adapter->Release();
    }
    factory->Release();
    return text.tellp() > 0 ? text.str() : "none reported";
}

std::string UiLanguage() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (::LCIDToLocaleName(::GetUserDefaultUILanguage(), name, LOCALE_NAME_MAX_LENGTH, 0) > 0) return Narrow(name);
    return "unknown";
}

// GDI+ is already linked for the keyboard icons: turn Aurora's BMP capture into
// a PNG so the report stays small.
bool ConvertBmpToPng(const fs::path& bmp, const fs::path& png) {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    {
        Gdiplus::Bitmap image(bmp.c_str());
        if (image.GetLastStatus() == Gdiplus::Ok) {
            const CLSID pngEncoder = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
            ok = image.Save(png.c_str(), &pngEncoder, nullptr) == Gdiplus::Ok;
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return ok;
}

void OpenInExplorer(const fs::path& file) {
    const std::wstring arguments = L"/select,\"" + file.wstring() + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}
#endif

bool WaitForStableFile(const fs::path& path, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::uintmax_t lastSize = 0;
    int stableTicks = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (!ec && size > 0) {
            if (size == lastSize && ++stableTicks >= 3) return true;
            lastSize = size;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    return false;
}

void Worker(std::string trigger) {
    const fs::path reportsRoot = RuntimeConfigFile::ApplicationDataDirectory() / "Reports";
    const std::string stamp = Timestamp();
    const std::string reportName = "report_" + stamp + "_v" + PRIME_PORT_VERSION;
    const fs::path folder = reportsRoot / reportName;
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        SetStatus("Could not create " + Utf8(folder) + ": " + ec.message());
        g_busy.store(false);
        return;
    }

    // 1. Screenshot of the frame currently presented (Aurora reads the GPU
    //    presentation source, so it matches what the player sees).
    const fs::path bmp = folder / "screenshot.bmp";
    const fs::path png = folder / "screenshot.png";
    aurora_request_frame_capture(0, Utf8(bmp).c_str()); // 0 = next presented frame
    bool screenshot = WaitForStableFile(bmp, std::chrono::milliseconds(4000));
#if defined(_WIN32)
    if (screenshot && ConvertBmpToPng(bmp, png)) fs::remove(bmp, ec);
#endif

    // 2. Logs of this run: transcript plus any crash artifacts already written.
    const fs::path runDirectory = RuntimeCrash::RunLogDirectory();
    const bool transcript = CopyInto(runDirectory / "console.log", folder);
    unsigned crashFiles = 0;
    for (const auto& entry : fs::directory_iterator(runDirectory, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        const std::string name = Utf8(entry.path().filename());
        if (name.rfind("crash_", 0) == 0 && entry.path().extension() == ".txt" && CopyInto(entry.path(), folder)) ++crashFiles;
    }

    // 3. Player settings.
    const bool config = CopyInto(RuntimeConfigFile::ResolveConfigPath(), folder, "Config.toml");

    // 3b. Metroid Prime 3 save data (NAND title/00010000/524d3345/data: save.bin
    //     slots + banner.bin), so a reported spot can be reloaded directly.
    //     Copied into save/, the same layout the player copies back to restore it.
    unsigned saveFiles = 0;
    {
        const fs::path saveDir = RuntimeNandPath::DiscoverNandRootPath() / "title" / "00010000" / "524d3345" / "data";
        const fs::path saveOut = folder / "save";
        for (const auto& entry : fs::directory_iterator(saveDir, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || fileEc) continue;
            if (saveFiles == 0) fs::create_directories(saveOut, fileEc);
            if (CopyInto(entry.path(), saveOut)) ++saveFiles;
        }
        ec.clear();
    }

    // 4. System summary.
    {
        std::ofstream summary(folder / "system.txt");
        summary << PRIME_PORT_NAME << " " << PRIME_PORT_VERSION << " (created by " << PRIME_PORT_AUTHOR << ")\n";
        summary << "report trigger: " << trigger << "\n";
        summary << "report time: " << stamp << "\n";
        if (const auto exe = RuntimeConfigFile::ExecutableDirectory()) summary << "executable directory: " << Utf8(*exe) << "\n";
        summary << "run log directory: " << Utf8(runDirectory) << "\n";
#if defined(_WIN32)
        summary << "os: " << WindowsVersion() << "\n";
        summary << "ui language: " << UiLanguage() << "\n";
        summary << "cpu: " << CpuBrand() << "\n";
        summary << "memory: " << MemoryGiB() << "\n";
        summary << "gpu: " << GpuAdapters() << "\n";
        summary << "primary display: " << ::GetSystemMetrics(SM_CXSCREEN) << "x" << ::GetSystemMetrics(SM_CYSCREEN) << "\n";
#endif
        summary << "graphics api: " << RuntimeConfigFile::GraphicsApi() << "\n";
        summary << "display mode: " << RuntimeConfigFile::DisplayMode("windowed") << "\n";
        summary << "keyboard/mouse mode: " << (RuntimeConfigFile::PrimeKeyboardMouseMode() ? "on" : "off") << "\n";
        summary << "disc image: " << Utf8(RuntimeConfigFile::ResolvedPrimeIsoPath()) << "\n";
        summary << "screenshot: " << (screenshot ? "yes" : "no") << ", transcript: " << (transcript ? "yes" : "no")
                << ", crash files: " << crashFiles << ", config: " << (config ? "yes" : "no")
                << ", save files: " << saveFiles << "\n";
    }
    {
        std::ofstream readme(folder / "HOW_TO_REPORT.txt");
        readme << PRIME_PORT_NAME << " version " << PRIME_PORT_VERSION << " - report " << stamp << "\n"
                  "Thank you for testing " << PRIME_PORT_NAME << "!\n\n"
                  "1. Open a new post in the Bug reports category at " << PRIME_PORT_REPOSITORY << "/discussions\n"
                  "2. Attach the .zip that sits next to this folder (drag it into the text box).\n"
                  "3. Tell us WHERE you were in the game (planet, room, what you were doing) and what went wrong.\n"
                  "   The area name is the most useful detail: with it the problem can be reproduced directly.\n\n"
                  "The zip contains only: a screenshot, this run's console.log, crash_*.txt if the game crashed,\n"
                  "your Config.toml (settings and key bindings), system.txt (Windows build, CPU, RAM, GPU) and\n"
                  "save/ with your Metroid Prime 3 save (save.bin, banner.bin) so the reported spot can be loaded.\n"
                  "No game data and no personal files are included; feel free to open the files first.\n";
    }

    // 5. Zip everything; the folder stays so the tester can inspect it.
    const fs::path zipPath = reportsRoot / (reportName + ".zip");
    {
        ZipWriter zip(zipPath);
        if (!zip.ok()) {
            SetStatus("Report folder written, zip failed: " + Utf8(zipPath));
            g_busy.store(false);
            return;
        }
        for (const auto& entry : fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || fileEc) continue;
            std::string relative = fs::relative(entry.path(), folder, fileEc).generic_string();
            if (fileEc || relative.empty()) relative = Utf8(entry.path().filename());
            zip.Add(reportName + "/" + relative, ReadAll(entry.path()));
        }
        zip.Finish();
    }
    RT_LOG(RT_TAG_RUNTIME) << "[bug-report] trigger=" << trigger << " zip=" << Utf8(zipPath)
                           << " screenshot=" << screenshot << " transcript=" << transcript
                           << " crash_files=" << crashFiles << " save_files=" << saveFiles << std::endl;
    SetStatus("Report saved: " + Utf8(zipPath));
#if defined(_WIN32)
    OpenInExplorer(zipPath);
#endif
    g_busy.store(false);
}

} // namespace

void Create(std::string_view trigger) {
    if (g_busy.exchange(true)) return;
    SetStatus("Writing report...");
    std::thread(Worker, std::string(trigger)).detach();
}

std::string Status() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_status;
}

std::string Directory() {
    return Utf8(RuntimeConfigFile::ApplicationDataDirectory() / "Reports");
}

} // namespace BugReport
