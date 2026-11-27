// Performance capture for a tester's machine: see include/perf_capture.h.
// Linux performance-capture builds only (MKW_PRIME_PERF_CAPTURE); empty elsewhere.
#if defined(__linux__) && defined(MKW_PRIME_PERF_CAPTURE)

#include "perf_capture.h"

#include "bug_report.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "stack_profiler.h"

#include <aurora/gfx.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace PerfCapture {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

const fs::file_time_type g_processStartFileTime = fs::file_time_type::clock::now();

bool g_active = false;
Clock::time_point g_startedAt{};
Clock::time_point g_finishedAt{};
Clock::time_point g_nextSample{};

fs::path ProfileDirectory() { return RuntimeConfigFile::ApplicationDataDirectory() / "Logs" / "profiles"; }

std::string ReadLine(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

long long ReadNumber(const fs::path& path, long long fallback = -1) {
    const std::string text = ReadLine(path);
    if (text.empty()) return fallback;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    return end == text.c_str() ? fallback : value;
}

// The amdgpu (or other) card that reports a load percentage: the one the game renders on for an APU.
fs::path GpuDevice() {
    static const fs::path device = [] {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;
            if (fs::exists(entry.path() / "device" / "gpu_busy_percent", ec)) return entry.path() / "device";
        }
        return fs::path{};
    }();
    return device;
}

// Current level of a pp_dpm_* table: the line marked with '*' ("1: 2700Mhz *").
std::string CurrentDpm(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find('*') == std::string::npos) continue;
        const auto colon = line.find(':');
        std::string value = colon == std::string::npos ? line : line.substr(colon + 1);
        value.erase(std::remove(value.begin(), value.end(), '*'), value.end());
        value.erase(std::remove(value.begin(), value.end(), ' '), value.end());
        return value;
    }
    return "?";
}

fs::path GpuHwmon() {
    static const fs::path hwmon = [] {
        std::error_code ec;
        const fs::path dir = GpuDevice() / "hwmon";
        if (GpuDevice().empty()) return fs::path{};
        for (const auto& entry : fs::directory_iterator(dir, ec)) return entry.path();
        return fs::path{};
    }();
    return hwmon;
}

// Average and highest current CPU clock (MHz) across logical CPUs.
void CpuClocks(long long& averageMhz, long long& maxMhz) {
    long long sum = 0, count = 0, best = 0;
    for (int cpu = 0; cpu < 256; ++cpu) {
        const long long khz =
            ReadNumber("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq");
        if (khz < 0) {
            if (cpu > 0) break;
            continue;
        }
        sum += khz;
        ++count;
        best = std::max(best, khz);
    }
    averageMhz = count ? sum / count / 1000 : -1;
    maxMhz = best / 1000;
}

// Whole-system CPU busy fraction since the previous call (/proc/stat).
double SystemCpuBusy() {
    static unsigned long long lastIdle = 0, lastTotal = 0;
    std::ifstream in("/proc/stat");
    std::string cpu;
    unsigned long long v[10] = {};
    in >> cpu;
    for (auto& x : v) in >> x;
    const unsigned long long idle = v[3] + v[4];
    unsigned long long total = 0;
    for (int i = 0; i < 8; ++i) total += v[i];
    const unsigned long long dIdle = idle - lastIdle, dTotal = total - lastTotal;
    const bool first = lastTotal == 0;
    lastIdle = idle;
    lastTotal = total;
    return first || dTotal == 0 ? -1.0 : 100.0 * double(dTotal - dIdle) / double(dTotal);
}

// Counters at the previous sample (reset when a capture starts).
uint64_t g_lastPresents = 0, g_lastEncodeNs = 0, g_lastPrepareNs = 0;
Clock::time_point g_lastSampleAt{};

void ResetSampleCounters() {
    g_lastPresents = aurora_get_present_count();
    aurora_get_frame_worker_totals(&g_lastEncodeNs, &g_lastPrepareNs);
    g_lastSampleAt = Clock::now();
}

void LogSystemSample() {
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - g_lastSampleAt).count();
    const uint64_t presents = aurora_get_present_count();
    uint64_t encodeNs = 0, prepareNs = 0;
    aurora_get_frame_worker_totals(&encodeNs, &prepareNs);
    const uint64_t frames = presents - g_lastPresents;
    // Per second of wall time, so a late sample does not read as a lower frame rate.
    const double perSecond = seconds > 0.05 ? 1.0 / seconds : 0.0;
    char rates[160];
    std::snprintf(rates, sizeof(rates), " fps=%.1f worker_encode_ms_per_s=%.0f worker_wait_gpu_ms_per_s=%.0f",
                  double(frames) * perSecond, double(encodeNs - g_lastEncodeNs) / 1e6 * perSecond,
                  double(prepareNs - g_lastPrepareNs) / 1e6 * perSecond);
    g_lastPresents = presents;
    g_lastEncodeNs = encodeNs;
    g_lastPrepareNs = prepareNs;
    g_lastSampleAt = now;
    long long cpuAvg = 0, cpuMax = 0;
    CpuClocks(cpuAvg, cpuMax);
    const double cpuBusy = SystemCpuBusy();
    std::ostringstream line;
    line << "[perf-sys] t=" << std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_startedAt).count()
         << "s" << rates;
    const fs::path gpu = GpuDevice();
    if (!gpu.empty()) {
        line << " gpu_busy=" << ReadNumber(gpu / "gpu_busy_percent") << "% sclk=" << CurrentDpm(gpu / "pp_dpm_sclk")
             << " mclk=" << CurrentDpm(gpu / "pp_dpm_mclk");
        const fs::path hwmon = GpuHwmon();
        if (!hwmon.empty()) {
            long long power = ReadNumber(hwmon / "power1_average");
            if (power < 0) power = ReadNumber(hwmon / "power1_input");
            if (power >= 0) line << " gpu_power=" << power / 1000000.0 << "W";
            const long long temp = ReadNumber(hwmon / "temp1_input");
            if (temp >= 0) line << " gpu_temp=" << temp / 1000 << "C";
        }
    } else {
        line << " gpu_busy=n/a";
    }
    char busy[32];
    std::snprintf(busy, sizeof(busy), "%.0f", cpuBusy);
    line << " cpu_mhz_avg=" << cpuAvg << " cpu_mhz_max=" << cpuMax << " system_cpu_busy=" << busy << "%";
    RT_LOG(RT_TAG_RUNTIME) << line.str() << std::endl;
}
}  // namespace

bool Start(std::vector<uint32_t> threadIds) {
    if (g_active) return false;
    if (!StackProfiler::Start(std::move(threadIds), kSeconds, ProfileDirectory())) return false;
    g_active = true;
    g_startedAt = Clock::now();
    g_nextSample = g_startedAt + std::chrono::seconds(1);
    ResetSampleCounters();
    SystemCpuBusy();  // primes the /proc/stat baseline
    aurora_set_gpu_timing_capture(true);
    RT_LOG(RT_TAG_RUNTIME) << "[perf-capture] started (" << kSeconds << " s)" << std::endl;
    std::istringstream power(PowerSummary());
    for (std::string line; std::getline(power, line);) RT_LOG(RT_TAG_RUNTIME) << "[perf-capture] " << line << std::endl;
    return true;
}

void Tick() {
    if (!g_active) return;
    const auto now = Clock::now();
    if (now >= g_nextSample) {
        g_nextSample += std::chrono::seconds(1);
        if (g_nextSample < now) g_nextSample = now + std::chrono::seconds(1);
        LogSystemSample();
    }
    if (StackProfiler::IsRunning()) return;
    g_active = false;
    g_finishedAt = now;
    aurora_set_gpu_timing_capture(false);
    RT_LOG(RT_TAG_RUNTIME) << "[perf-capture] finished: " << StackProfiler::Status() << std::endl;
    BugReport::Create("F8 performance capture");
}

void DrawNotice() {
    const auto now = Clock::now();
    const bool showDone = !g_active && g_finishedAt.time_since_epoch().count() != 0 &&
                          now - g_finishedAt < std::chrono::seconds(10);
    if (!g_active && !showDone) return;
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (ImGui::Begin("##perf_capture", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav)) {
        if (g_active) {
            const float elapsed = std::chrono::duration<float>(now - g_startedAt).count();
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Recording performance capture: %.0f / %.0f s",
                               std::min(elapsed, float(kSeconds)), kSeconds);
            ImGui::TextUnformatted("Keep playing where the game is slow.");
        } else {
            ImGui::TextUnformatted("Performance capture finished.");
            ImGui::TextUnformatted(BugReport::Status().c_str());
        }
    }
    ImGui::End();
}

std::string PowerSummary() {
    std::ostringstream out;
    const auto add = [&](const char* key, const std::string& value) {
        if (!value.empty()) out << key << ": " << value << "\n";
    };
    add("cpu governor", ReadLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"));
    add("cpu energy preference", ReadLine("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"));
    add("cpu scaling driver", ReadLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver"));
    {
        const long long maxKhz = ReadNumber("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq");
        if (maxKhz > 0) add("cpu max clock allowed", std::to_string(maxKhz / 1000) + " MHz");
    }
    add("platform profile", ReadLine("/sys/firmware/acpi/platform_profile"));
    {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator("/sys/class/power_supply", ec)) {
            const std::string type = ReadLine(entry.path() / "type");
            if (type == "Mains") add("ac power", ReadNumber(entry.path() / "online") == 1 ? "plugged in" : "on battery");
            if (type == "Battery") {
                const long long capacity = ReadNumber(entry.path() / "capacity");
                if (capacity >= 0)
                    add("battery", std::to_string(capacity) + "% " + ReadLine(entry.path() / "status"));
            }
        }
    }
    const fs::path gpu = GpuDevice();
    if (!gpu.empty()) {
        add("gpu performance level", ReadLine(gpu / "power_dpm_force_performance_level"));
        std::ifstream profile(gpu / "pp_power_profile_mode");
        for (std::string line; std::getline(profile, line);) {
            if (line.find('*') == std::string::npos) continue;
            line.erase(std::remove(line.begin(), line.end(), '*'), line.end());
            const auto first = line.find_first_not_of(' ');
            if (first != std::string::npos) add("gpu power profile", line.substr(first));
            break;
        }
        add("gpu clock now", CurrentDpm(gpu / "pp_dpm_sclk"));
        const long long vram = ReadNumber(gpu / "mem_info_vram_total");
        if (vram > 0) add("gpu dedicated memory", std::to_string(vram >> 20) + " MiB");
        const long long gtt = ReadNumber(gpu / "mem_info_gtt_total");
        if (gtt > 0) add("gpu shared memory (gtt)", std::to_string(gtt >> 20) + " MiB");
    }
    return out.str();
}

std::vector<fs::path> ProfilesOfThisRun() {
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(ProfileDirectory(), ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("stack_profile_", 0) != 0) continue;
        std::error_code timeError;
        const auto written = fs::last_write_time(entry.path(), timeError);
        if (!timeError && written >= g_processStartFileTime) found.emplace_back(written, entry.path());
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<fs::path> paths;
    for (auto& [_, path] : found) paths.push_back(std::move(path));
    return paths;
}
}  // namespace PerfCapture

#endif
