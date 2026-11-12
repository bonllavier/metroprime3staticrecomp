#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <toml.hpp>
#include "platform/host_platform.h"
#include "hle/prime_disc_path.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

struct RuntimeUserConfig {
    std::optional<bool> widescreen;
    std::optional<bool> forceAspect169;
    std::optional<std::string> primeAspectRatio;  // [video] aspect_ratio: "16:9", "window", "21:9", "32:9", ...
    std::optional<int32_t> windowPosX;
    std::optional<int32_t> windowPosY;
    std::optional<uint32_t> windowWidth;
    std::optional<uint32_t> windowHeight;
    std::optional<float> resolutionMultiplier;
    std::optional<std::string> graphicsApi;
    std::optional<std::string> displayMode;
    std::optional<uint32_t> frameInterpolationFps;
    // [video] frame_interpolation_low_latency: show each real frame at once, inserted images predicted past it.
    std::optional<bool> frameInterpolationLowLatency;
    // [video] vsync: present in step with the display's refresh (no tearing, presentation capped at the
    // refresh rate). Off by default, as before the option existed.
    std::optional<bool> vsync;
    std::optional<bool> frameInterpolationDisplaySync;
    std::optional<bool> metalFxSpatialUpscaling;
    std::optional<bool> skipUnreadyPipelines;
    std::optional<bool> ubershader;  // hybrid ubershader: draw with it while a pipeline compiles (on by default)
    std::optional<bool> disableCopyFilter;
    std::optional<bool> textureReplacements;
    std::optional<bool> textureDumps;
    std::optional<uint32_t> msaa;                   // 1, 2, 4, 8 (restart)
    std::optional<uint32_t> anisotropicFiltering;   // 0 = as the game requests, 2, 4, 8, 16
    std::optional<bool> perPixelLighting;           // GX lighting per fragment (live)
    std::optional<bool> enhancedMetal;              // metal reflections/highlights (needs per-pixel lighting)
    std::optional<bool> enhancedMaterials;          // per-texture material classes (ship hulls); per-pixel lighting
    // Modern lighting, level 1 (F1 > Video; docs/iluminacion_motor.md): a master switch and one per part.
    std::optional<bool> modernLighting, modernEngineLights, modernExtraDynamicLights, modernContactShadows,
        modernTemporal, modernActorLightFade, modernLightmapDither, modernAutoExposure;
    std::optional<float> modernContactStrength, modernContactLength, modernExposureRange, modernExposureSpeed;
    std::optional<float> enhancedMetalStrength;     // 0..2
    std::optional<float> enhancedMetalRoughness, enhancedMetalMetallic, enhancedMetalEdge, enhancedMetalDetail;
    std::optional<uint32_t> textureUpscale;         // 0 = off, 2, 4 (Super-xBR at load)
    std::optional<uint32_t> textureUpscaleMaxSize;  // 128, 256, 512, 1024
    std::optional<float> textureUpscaleSharpen;     // 0..1 unsharp mask after the upscale
    std::optional<bool> showFps;
    std::optional<uint32_t> disabledPostProcessingPaths;
    std::optional<float> audioVolume;
    std::optional<float> audioMusicVolume;
    std::optional<float> audioSoundEffectsVolume;
    std::optional<float> audioUiVolume;
    std::optional<float> audioVoicesVolume;
    std::optional<bool> audioMuted;
    std::optional<bool> audioMixWorker;
    std::optional<bool> attenuateMusicWhenMediaPlays;
    // [prime]: Metroid Prime 3 host options (see settings_overlay's F1 panel).
    std::optional<std::string> primeSoundMode;
    std::optional<std::string> primeLanguage;        // english, spanish, french, german, italian
    std::optional<bool> primeHideCursor;
    std::optional<bool> primeMouseLook;              // camera follows the mouse in first person
    std::optional<float> primeMouseLookSensitivity;  // degrees per pixel
    std::optional<bool> primeMouseLookInvertY;
    std::optional<float> primeMouseLookSmoothing;    // 0 = none
    std::optional<float> primeMouseLookHintScale, primeMouseLookHintX, primeMouseLookHintY;  // on-screen hint geometry
    std::optional<bool> primeRawPointer;         // bypass the game's pointer smoothing (keyboard/mouse mode)
    std::optional<bool> primeKeyboardMouse;      // virtual Wii Remote + Nunchuk driven by keyboard and mouse (startup)
    std::optional<bool> primePresentQueueLimit;  // DXGI swap chain queue limited to one frame
    std::optional<bool> primeGpuThread;
    std::optional<bool> primeAuroraDecodeThread;
    std::optional<bool> primeOverlapEncode;
    std::optional<bool> primeLazyEndDrain;
    // Stage 2 of the asynchronous decoder: packed display-list draws expanded on the decoder thread.
    std::optional<bool> primeDeferDlExpansion;
    std::optional<bool> primeDeferDlVerify;  // diagnostics: compare the decoder expansion with the GX thread's
    std::optional<bool> primeDlVertexCache;  // false: WII_COMPILED_DISABLE_DL_VERTEX_CACHE (A/B)
    std::optional<bool> primePresentOnCopy;
    std::optional<bool> primeHostAudioResampler;
    std::optional<bool> primeParallelTextureDecode;
    std::optional<bool> primeShowPerformancePanel;
    std::optional<bool> primeCheckUpdates;         // ask GitHub Releases for a newer build at startup
    // [effects]: Aurora image effects on the presented frame (F1 > Effects, all live).
    std::optional<bool> effectsSharpen, effectsFxaa, effectsBloom, effectsSsao, effectsColor, effectsBloomWorldOnly;
    std::optional<bool> effectsSsaoSkipFace;
    // World lighting (experimental, F1 > Effects), each level on its own: environment material on
    // lightmapped walls and floors, borrowed lights for their highlights, floor reflections (SSR).
    std::optional<bool> effectsEnvMaterial, effectsEnvLights, effectsSsr;
    std::optional<float> effectsSsrStrength, effectsSsrRoughness, effectsSsrRelief;
    // More lighting improvements (F1 > Effects > World lighting), each on its own.
    std::optional<bool> effectsLitRooms, effectsSoftHighlights, effectsLinearLighting, effectsSmoothLightmaps,
        effectsAreaReflections, effectsSoftShadows, effectsSpecOcclusion, effectsLightShafts;
    std::optional<float> effectsLightShaftsIntensity;
    std::optional<bool> effectsGlowFix;  // [effects] glow_ghost_fix
    std::optional<bool> effectsWallSsr;  // [effects] wall_reflections (experimental)
    std::optional<float> effectsWallSsrStrength;
    std::optional<float> effectsSharpenAmount, effectsBloomThreshold, effectsBloomIntensity, effectsBloomRadius;
    std::optional<float> effectsSsaoRadius, effectsSsaoIntensity, effectsSsaoSoftness;
    std::optional<float> effectsSsaoSamples, effectsSsaoBias, effectsSsaoPower, effectsSsaoFade, effectsSsaoEdge;
    std::optional<float> effectsSsaoGlow;
    std::optional<float> effectsBrightness, effectsContrast, effectsSaturation, effectsGamma, effectsTemperature, effectsVignette;
    // [reshade]: load the bundled ReShade DLL before the renderer starts (startup).
    std::optional<bool> reshadeEnabled;
    std::optional<bool> primeFastDiscReads;
    std::optional<bool> primeAsyncDiscReads;
    std::optional<bool> primeGpuTextureDecode;
    std::optional<bool> primeKeyboardIcons;      // key caps replace the Wii button textures (startup)
    std::optional<bool> primeWiimoteBatteryFull; // WPADGetInfoAsync answers battery 4/4, never low (live)
    // [prime_keys]: keyboard/mouse bindings per Wii action, e.g. a = ["Mouse Left", "Return"].
    std::map<std::string, std::vector<std::string>> primeKeyBindings;
    // [prime_pad]: gamepad bindings per Wii action, positional SDL3 names, e.g. a = ["right_trigger"].
    std::map<std::string, std::vector<std::string>> primePadBindings;
    // [prime_pad_motion]: the input that drives each action while the motion
    // button is held (prime_gamepad.h, motion layer), e.g. lasso_pull = ["north"].
    std::map<std::string, std::vector<std::string>> primePadLayerBindings;
    std::optional<bool> primeGamepad;             // SDL3 gamepads drive the virtual remote too (live)
    std::optional<float> primePadLookSpeed;       // right stick: degrees per second at full deflection
    std::optional<float> primePadLookCurve;       // right stick response exponent (1 = linear)
    std::optional<float> primePadPointerSpeed;    // right stick: screen widths per second for the pointer
    std::optional<float> primePadDeadzone;        // inner radial dead zone of both sticks
    std::optional<bool> primePadInvertY;
    std::optional<bool> primePadIcons;            // prompts show the pad's symbols while it is in use
    // First-person field of view (hle/prime_fov.h, after PrimeHack's view modifier).
    std::optional<bool> primeFovEnabled;          // write the chosen FOV into the active camera
    std::optional<float> primeFov;                // degrees, 40..170
    std::optional<bool> primeFovCullingOff;       // draw everything even at or below 94 degrees
    std::optional<bool> primeFovArmAdjust;        // move the arm cannon in and lower z-near as the FOV widens
    // RetroAchievements, casual mode (hle/prime_achievements.h). The session token
    // is not here: it lives DPAPI-encrypted in UserData/retroachievements.dat.
    std::optional<bool> primeAchievements;
    // [prime_materials]: F1 > Materials values, <entry>_<parameter> = number (hle/prime_materials.h).
    std::map<std::string, float> primeMaterials;
    std::optional<bool> primeAchievementsNotifications;
    // Real Wii Remotes (with or without Nunchuk / Classic Controller) and Wii U Pro
    // Controllers paired over Bluetooth, driven by SDL's HIDAPI Wii driver. The driver
    // is opt-in on SDL's side, so this decides whether the runtime turns it on.
    std::optional<bool> wiiRemotes;
    // Keep re-enumerating Bluetooth HID devices while no Wii controller is connected
    // (Dolphin's "continuous scanning"), so a remote that dropped or was switched on
    // after launch shows up without restarting.
    std::optional<bool> wiiContinuousScan;
    // Accelerometer zero-point correction for the Bluetooth Wii Remote, in g and in
    // SDL's sensor frame (x right, y out of the button face, z towards the user).
    // SDL's Wii driver falls back to a nominal zero point when its read of the
    // remote's calibration block times out (common over Bluetooth), so this is
    // measured in the overlay with the remote at rest. Stored per remote under
    // [controller.wii_accel_offsets]; the old shared
    // wii_accel_offset_* keys are only detected, to report that they are ignored.
    std::map<std::string, std::array<double, 3>> wiiAccelOffsets;
    bool hasLegacyWiiAccelOffset = false;
    // Debugging aid: append every KPAD sample of the Bluetooth remote (raw and
    // corrected accelerometer, buttons) to wii_accel_trace.csv next to Config.toml.
    std::optional<bool> wiiAccelTrace;
    std::optional<bool> networkEnabled;
    std::optional<bool> discordPresenceEnabled;
    // The application ID of the WiiCompiled Discord application. This is only
    // used by the base product; Retro Rewind supplies its own ID through the
    // standard Dolphin /dev/dolphin interface.
    std::optional<std::string> discordClientId;
    std::optional<std::string> nandRoot;
    std::optional<std::string> dvdRoot;
    std::optional<std::string> primeIsoPath;
    // Where F5 draw captures go (empty: UserData/Logs/captures). Captures are large; another drive helps.
    std::optional<std::string> captureDir;
    // The one canonical Retro Rewind installation, owned and updated by the frontend. Setup records
    // it here instead of copying the pack, so an asset-only update is visible on the next launch.
    std::optional<std::string> retroRewindRoot;
    std::vector<std::string> overlayRoots;
    // Controller mappings use Wii/GameCube button names as keys and up to two
    // comma-separated SDL-style physical button names ("south", or
    // "dpad_up,left_shoulder") as values; pressing either bound button counts.
    std::array<std::optional<std::string>, 12> controllerButtons;
    std::optional<bool> rumbleEnabled;
    std::optional<int32_t> muteHotkey;
    std::map<std::string, std::string> controllerExpressions;
};

namespace RuntimeConfigFile {

// Narrow path strings are UTF-8 everywhere in the runtime; string() and the
// char path constructor would use the ANSI codepage on Windows, which drops
// characters the codepage cannot represent.
inline std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline constexpr const char* kConfigFileName = "Config.toml";
inline constexpr const char* kApplicationDirectoryName = "WiiCompiled";

// Portable layout. A directory holding kPortableMarkerFileName is a portable root; every piece of
// runtime user state (Config.toml, NAND, Cache, Logs) lives in <root>/UserData instead of
// %LOCALAPPDATA%. The marker is searched for from the executable's directory upwards, which is what
// makes an installation survive being moved or carried on removable media.
inline constexpr const char* kPortableMarkerFileName = "portable.txt";
inline constexpr const char* kPortableUserDataDirectoryName = "UserData";

// The installed layout puts products two levels below the root (<root>/Install/Base/game.exe). The
// bound is deliberately small so an unrelated marker far up a drive can never capture an ordinary
// installation.
inline constexpr int kPortableSearchDepth = 4;

inline std::string Trim(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

inline bool IsSupportedMsaa(uint32_t value) {
    return value == 1 || value == 2 || value == 4 || value == 8;
}
inline bool IsSupportedTextureUpscale(uint32_t value) {
    return value == 0 || value == 2 || value == 4;
}
inline bool IsSupportedTextureUpscaleMaxSize(uint32_t value) {
    return value == 128 || value == 256 || value == 512 || value == 1024;
}
inline bool IsSupportedAnisotropicFiltering(uint32_t value) {
    return value == 0 || value == 2 || value == 4 || value == 8 || value == 16;
}

inline bool IsSupportedResolutionMultiplier(float value) {
    static constexpr std::array values{0.0f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Must stay in step with the backend table in main.cpp, which is what actually
// maps these to AuroraBackend.
inline bool IsSupportedGraphicsApi(std::string_view value) {
#if defined(__APPLE__)
    static constexpr std::array<std::string_view, 2> values{"auto", "metal"};
// only vulkan for linux
#elif defined(__linux__)
    static constexpr std::array<std::string_view, 2> values{"auto", "vulkan"};
#elif defined(_WIN32)
    static constexpr std::array<std::string_view, 3> values{"auto", "d3d12", "vulkan"};
#endif
    return std::find(values.begin(), values.end(), value) != values.end();
}

inline bool IsSupportedDisplayMode(std::string_view value) {
    static constexpr std::array<std::string_view, 3> values{
        "windowed", "borderless", "exclusive",
    };
    return std::find(values.begin(), values.end(), value) != values.end();
}

// [video] frame_interpolation_fps: 0 off, 1 the monitor's refresh rate, 2 uncapped, otherwise the
// images per second offered in F1 > Video.
constexpr uint32_t kFrameInterpolationMatchMonitor = 1;
constexpr uint32_t kFrameInterpolationUncapped = 2;
constexpr std::array<uint32_t, 9> kFrameInterpolationChoices{0, 1, 120, 144, 165, 180, 240, 360, 2};

inline bool IsSupportedFrameInterpolationFps(uint32_t value) {
    return std::find(kFrameInterpolationChoices.begin(), kFrameInterpolationChoices.end(), value) !=
           kFrameInterpolationChoices.end();
}

inline std::optional<std::filesystem::path> ExecutableDirectory() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    return RuntimePlatform::ExecutableDirectory();
#else
    // /proc/self/exe is a Linux-specific magic symlink to the running executable; readlink()
    // does not NUL-terminate and silently truncates if the buffer is too small, so this grows
    // the buffer until the result no longer fills it completely, the same doubling strategy as
    // the Windows branch above uses for GetModuleFileNameW.
    std::string buffer(256, '\0');
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return std::nullopt;
        }
        if (static_cast<size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<size_t>(length));
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

// The portable root this executable lives under, or nullopt for a normal installation. The answer
// cannot change while the process runs, so it is resolved exactly once: every user-state path
// derives from it and they must not disagree with each other.
inline const std::optional<std::filesystem::path>& PortableRootDirectory() {
    static const std::optional<std::filesystem::path> root = []() -> std::optional<std::filesystem::path> {
        const auto executableDirectory = ExecutableDirectory();
        if (!executableDirectory) {
            return std::nullopt;
        }
        std::filesystem::path current = *executableDirectory;
        for (int level = 0; level <= kPortableSearchDepth; ++level) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(current / kPortableMarkerFileName, ec)) {
                return current;
            }
            const auto parent = current.parent_path();
            if (parent.empty() || parent == current) {
                break;
            }
            current = parent;
        }
        return std::nullopt;
    }();
    return root;
}

inline std::filesystem::path ApplicationDataDirectory() {
#if MKW_PRIME_QA_TOOLS
    // Test build: a replay runs against its own profile (save, settings, caches) so it never touches
    // the player's. See recomp/qa/README.md.
    if (const char* qaDir = std::getenv("WII_COMPILED_QA_USER_DATA"); qaDir && *qaDir) {
        return std::filesystem::path(qaDir);
    }
#endif
    if (const auto& portableRoot = PortableRootDirectory()) {
        return *portableRoot / kPortableUserDataDirectoryName;
    }
#ifdef _WIN32
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath)) && rawPath) {
        const std::filesystem::path directory = std::filesystem::path(rawPath) / kApplicationDirectoryName;
        CoTaskMemFree(rawPath);
        return directory;
    }
#elif defined(__APPLE__)
    return RuntimePlatform::ApplicationDataDirectory(kApplicationDirectoryName);
#else
    // XDG Base Directory spec equivalent of FOLDERID_LocalAppData: $XDG_DATA_HOME if set and
    // non-empty, otherwise its default of $HOME/.local/share.
    if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME"); xdgDataHome && *xdgDataHome) {
        return std::filesystem::path(xdgDataHome) / kApplicationDirectoryName;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / kApplicationDirectoryName;
    }
#endif
    return std::filesystem::current_path() / kApplicationDirectoryName;
}

inline std::filesystem::path ResolveConfigPath() {
    return ApplicationDataDirectory() / kConfigFileName;
}

inline void EnsureConfigFile() {
    const std::filesystem::path path = ResolveConfigPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec || std::filesystem::exists(path, ec)) {
        return;
    }

    if (const auto& portableRoot = PortableRootDirectory()) {
        std::error_code copyEc;
        const auto shipped = *portableRoot / "Config.default.toml";
        if (std::filesystem::is_regular_file(shipped, copyEc) && !copyEc &&
            std::filesystem::copy_file(shipped, path, copyEc) && !copyEc) {
            return;
        }
    }
    std::ofstream output(path);
    if (!output) {
        return;
    }
    output << "# WiiCompiled user configuration\n"
              "# Set paths.dvd_root to an extracted Mario Kart Wii DATA directory.\n\n"
              "[video]\n"
              "widescreen = true\n"
              "force_16_9 = false\n"
              "resolution_multiplier = 1.0\n"
              "frame_interpolation_fps = 0\n"
              "vsync = false\n"
              "display_mode = \"windowed\"\n"
              "graphics_api = \"auto\"\n"
#if defined(MKW_GAME_PROFILE_RM3E01_00)
              "skip_unready_pipelines = false\n"
#else
              "skip_unready_pipelines = true\n"
#endif
              "# Hybrid ubershader: while a material's shader compiles, draw with a generic one that\n"
              "# reads the material from memory instead of waiting. Compiled shaders keep priority.\n"
              "ubershader = true\n"
              "disable_copy_filter = true\n"
              "show_fps = true\n"
              "# Dolphin-style custom textures. When enabled, the renderer indexes\n"
              "# texture_replacements/ next to this file at startup and substitutes\n"
              "# any tex1_<W>x<H>_<hash>[_<tlut hash>]_<format>.dds or .png it finds\n"
              "# there for the matching game texture. texture_dumps writes every\n"
              "# unmatched texture to Cache/texture_dumps under the name a\n"
              "# replacement would need. Both are read once, at startup.\n"
              "texture_replacements = false\n"
              "texture_dumps = false\n"
              "# Antialiasing samples (1, 2, 4, 8; read at startup) and anisotropic\n"
              "# filtering (0 = as the game requests, 2, 4, 8, 16).\n"
              "msaa = 1\n"
              "anisotropic_filtering = 0\n"
              "# GX lighting evaluated per pixel instead of per vertex (smooth\n"
              "# dynamic lights and specular; same formula). Applies at once.\n"
              "per_pixel_lighting = false\n"
              "# Load-time texture upscaling (Super-xBR, CPU, no files involved):\n"
              "# 0 = off, 2 or 4. Only textures up to texture_upscale_max_size\n"
              "# (128, 256, 512, 1024) on each side are processed.\n"
              "texture_upscale = 0\n"
              "texture_upscale_max_size = 256\n"
              "# Sharpening (0.0 none .. 1.0 strong) applied to upscaled textures.\n"
              "texture_upscale_sharpen = 0.0\n\n"
              "[audio]\n"
              "volume = 1.0\n"
              "music_volume = 1.0\n"
              "sound_effects_volume = 1.0\n"
              "ui_volume = 1.0\n"
              "voices_volume = 1.0\n"
              "muted = false\n"
              "attenuate_music_when_media_plays = false\n"
              "# Runs the AX/DSP voice mix on its own thread, joined before the\n"
              "# guest can observe it. Set to false to mix inline on the guest\n"
              "# thread exactly as the runtime did before.\n"
              "mix_worker = true\n\n"
              "[network]\n"
              "enabled = true\n\n"
              "[discord]\n"
              "# Rich Presence talks only to a locally-running Discord client.\n"
              "# Retro Rewind supplies its official app ID automatically. Set this\n"
              "# to WiiCompiled's Discord application ID for basic base-game presence.\n"
              "enabled = true\n"
              "# client_id = \"123456789012345678\"\n\n"
              "[paths]\n"
              "# dvd_root = \"D:\\\\MarioKartWii\\\\DATA\"\n"
              "# nand_root = \"D:\\\\WiiNand\"\n"
              "# retro_rewind_root = \"D:\\\\RetroRewind\\\\RetroRewind6\"\n"
              "# overlay_roots = [\"D:\\\\RetroRewind\"]\n";
}

template <typename T>
inline std::optional<T> FindConfigValue(
    const toml::value& document, std::string_view section, std::string_view key) {
    try {
        return toml::find<T>(document, std::string(section), std::string(key));
    } catch (const std::exception&) {
    }
    // A whole number is a TOML integer ("ssao_samples = 8"): accept it for decimal settings too.
    // F1 used to save such values without a decimal point and they fell back to the default on the
    // next start (SSAO quality kept returning to 12, 2026-11-01).
    if constexpr (std::is_floating_point_v<T>) {
        try {
            return static_cast<T>(toml::find<int64_t>(document, std::string(section), std::string(key)));
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

inline std::optional<uint32_t> FindConfigUint(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < 0 || static_cast<uint64_t>(*value) > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*value);
}

inline std::optional<int32_t> FindConfigInt(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < INT32_MIN || *value > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<int32_t>(*value);
}

inline std::optional<float> FindConfigFloat(
    const toml::value& document, std::string_view section, std::string_view key) {
    std::optional<double> value = FindConfigValue<double>(document, section, key);
    if (!value) {
        if (const auto integer = FindConfigValue<int64_t>(document, section, key)) {
            value = static_cast<double>(*integer);
        }
    }
    if (!value || !std::isfinite(*value) ||
        *value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        *value > static_cast<double>(std::numeric_limits<float>::max())) {
        return std::nullopt;
    }
    return static_cast<float>(*value);
}

inline void AppendOverlayRoots(RuntimeUserConfig& config, const std::string& roots) {
    size_t begin = 0;
    while (begin < roots.size()) {
        const size_t end = roots.find(';', begin);
        std::string root = Trim(std::string_view(roots).substr(begin, end - begin));
        if (!root.empty()) {
            config.overlayRoots.push_back(std::move(root));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
}

// Reads every supported setting out of a parsed Config.toml document.
// [x, y, z] of finite numbers (integers allowed), else nullopt.
inline std::optional<std::array<double, 3>> ParseAccelOffset(const toml::value& value) {
    if (!value.is_array() || value.as_array().size() != 3) {
        return std::nullopt;
    }
    std::array<double, 3> offset{};
    for (size_t i = 0; i < 3; ++i) {
        const toml::value& element = value.as_array()[i];
        if (element.is_floating()) {
            offset[i] = element.as_floating();
        } else if (element.is_integer()) {
            offset[i] = static_cast<double>(element.as_integer());
        } else {
            return std::nullopt;
        }
        if (!std::isfinite(offset[i])) {
            return std::nullopt;
        }
    }
    return offset;
}

inline RuntimeUserConfig ParseConfigDocument(const toml::value& document) {
    RuntimeUserConfig config;

    static constexpr std::array<std::string_view, 12> buttonKeys = {
        "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
    };
    for (size_t index = 0; index < buttonKeys.size(); ++index) {
        config.controllerButtons[index] =
            FindConfigValue<std::string>(document, "controller", buttonKeys[index]);
    }

    config.rumbleEnabled = FindConfigValue<bool>(document, "controller", "rumble");
    if (auto value = FindConfigInt(document, "audio", "mute_key")) {
        config.muteHotkey = *value;
    }

    if (const auto* section = document.contains("controller") ? &document.at("controller") : nullptr;
        section != nullptr && section->is_table()) {
        for (const auto& [key, value] : section->as_table()) {
            if (key.rfind("expr_", 0) == 0 && value.is_string()) {
                config.controllerExpressions[key] = value.as_string();
            }
        }
        if (section->contains("wii_accel_offsets") && section->at("wii_accel_offsets").is_table()) {
            for (const auto& [key, value] : section->at("wii_accel_offsets").as_table()) {
                if (const auto offset = ParseAccelOffset(value)) {
                    config.wiiAccelOffsets[key] = *offset;
                }
            }
        }
    }

    config.widescreen = FindConfigValue<bool>(document, "video", "widescreen");
    if (auto value = FindConfigValue<std::string>(document, "prime", "sound_mode");
        value && (*value == "stereo" || *value == "surround" || *value == "mono")) {
        config.primeSoundMode = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "prime", "language");
        value && (*value == "english" || *value == "spanish" || *value == "french" || *value == "german" ||
                  *value == "italian")) {
        config.primeLanguage = *value;
    }
    config.primeHideCursor = FindConfigValue<bool>(document, "prime", "hide_cursor");
    config.primeMouseLook = FindConfigValue<bool>(document, "prime", "mouse_look");
    config.primeMouseLookSensitivity = FindConfigFloat(document, "prime", "mouse_look_sensitivity");
    config.primeMouseLookInvertY = FindConfigValue<bool>(document, "prime", "mouse_look_invert_y");
    config.primeMouseLookSmoothing = FindConfigFloat(document, "prime", "mouse_look_smoothing");
    config.primeMouseLookHintScale = FindConfigFloat(document, "prime", "mouse_look_hint_scale");
    config.primeMouseLookHintX = FindConfigFloat(document, "prime", "mouse_look_hint_x");
    config.primeMouseLookHintY = FindConfigFloat(document, "prime", "mouse_look_hint_y");
    config.primeRawPointer = FindConfigValue<bool>(document, "prime", "raw_pointer");
    config.primeKeyboardMouse = FindConfigValue<bool>(document, "prime", "keyboard_mouse");
    config.primePresentQueueLimit = FindConfigValue<bool>(document, "prime", "present_queue_limit");
    config.primeGpuThread = FindConfigValue<bool>(document, "prime", "gpu_thread");
    config.primeAuroraDecodeThread = FindConfigValue<bool>(document, "prime", "aurora_decode_thread");
    config.primeOverlapEncode = FindConfigValue<bool>(document, "prime", "overlap_encode");
    config.primeLazyEndDrain = FindConfigValue<bool>(document, "prime", "lazy_end_drain");
    config.primeDeferDlExpansion = FindConfigValue<bool>(document, "prime", "defer_dl_expansion");
    config.primeDeferDlVerify = FindConfigValue<bool>(document, "prime", "defer_dl_verify");
    config.primeDlVertexCache = FindConfigValue<bool>(document, "prime", "dl_vertex_cache");
    config.primePresentOnCopy = FindConfigValue<bool>(document, "prime", "present_on_copy");
    config.primeHostAudioResampler = FindConfigValue<bool>(document, "prime", "host_audio_resampler");
    config.primeParallelTextureDecode = FindConfigValue<bool>(document, "prime", "parallel_texture_decode");
    config.primeShowPerformancePanel = FindConfigValue<bool>(document, "prime", "show_performance_panel");
    config.primeCheckUpdates = FindConfigValue<bool>(document, "prime", "check_updates");
    config.effectsSharpen = FindConfigValue<bool>(document, "effects", "sharpen");
    config.effectsSharpenAmount = FindConfigValue<float>(document, "effects", "sharpen_amount");
    config.effectsFxaa = FindConfigValue<bool>(document, "effects", "fxaa");
    config.effectsBloom = FindConfigValue<bool>(document, "effects", "bloom");
    config.effectsBloomWorldOnly = FindConfigValue<bool>(document, "effects", "bloom_world_only");
    config.effectsBloomThreshold = FindConfigValue<float>(document, "effects", "bloom_threshold");
    config.effectsBloomIntensity = FindConfigValue<float>(document, "effects", "bloom_intensity");
    config.effectsBloomRadius = FindConfigValue<float>(document, "effects", "bloom_radius");
    config.effectsSsao = FindConfigValue<bool>(document, "effects", "ssao");
    config.effectsSsaoRadius = FindConfigValue<float>(document, "effects", "ssao_radius");
    config.effectsSsaoIntensity = FindConfigValue<float>(document, "effects", "ssao_intensity");
    config.effectsSsaoSoftness = FindConfigValue<float>(document, "effects", "ssao_softness");
    config.effectsSsaoSamples = FindConfigValue<float>(document, "effects", "ssao_samples");
    config.effectsSsaoBias = FindConfigValue<float>(document, "effects", "ssao_bias");
    config.effectsSsaoPower = FindConfigValue<float>(document, "effects", "ssao_power");
    config.effectsSsaoFade = FindConfigValue<float>(document, "effects", "ssao_fade_distance");
    config.effectsSsaoEdge = FindConfigValue<float>(document, "effects", "ssao_edge_sharpness");
    config.effectsSsaoGlow = FindConfigValue<float>(document, "effects", "ssao_glow_protect");
    config.effectsSsaoSkipFace = FindConfigValue<bool>(document, "effects", "ssao_skip_face");
    config.effectsEnvMaterial = FindConfigValue<bool>(document, "effects", "env_material");
    config.effectsEnvLights = FindConfigValue<bool>(document, "effects", "env_borrowed_lights");
    config.effectsSsr = FindConfigValue<bool>(document, "effects", "ssr");
    config.effectsSsrStrength = FindConfigValue<float>(document, "effects", "ssr_strength");
    config.effectsSsrRoughness = FindConfigValue<float>(document, "effects", "ssr_roughness");
    config.effectsSsrRelief = FindConfigValue<float>(document, "effects", "ssr_relief");
    config.effectsLitRooms = FindConfigValue<bool>(document, "effects", "lit_room_material");
    config.effectsSoftHighlights = FindConfigValue<bool>(document, "effects", "soft_highlights");
    config.effectsLinearLighting = FindConfigValue<bool>(document, "effects", "linear_lighting");
    config.effectsSmoothLightmaps = FindConfigValue<bool>(document, "effects", "smooth_lightmaps");
    config.effectsAreaReflections = FindConfigValue<bool>(document, "effects", "area_reflections");
    config.effectsSoftShadows = FindConfigValue<bool>(document, "effects", "soft_shadows");
    config.effectsSpecOcclusion = FindConfigValue<bool>(document, "effects", "specular_occlusion");
    config.effectsLightShafts = FindConfigValue<bool>(document, "effects", "light_shafts");
    config.effectsLightShaftsIntensity = FindConfigValue<float>(document, "effects", "light_shafts_intensity");
    config.effectsGlowFix = FindConfigValue<bool>(document, "effects", "glow_ghost_fix");
    config.effectsWallSsr = FindConfigValue<bool>(document, "effects", "wall_reflections");
    config.effectsWallSsrStrength = FindConfigValue<float>(document, "effects", "wall_reflections_strength");
    config.effectsColor = FindConfigValue<bool>(document, "effects", "color");
    config.effectsBrightness = FindConfigValue<float>(document, "effects", "brightness");
    config.effectsContrast = FindConfigValue<float>(document, "effects", "contrast");
    config.effectsSaturation = FindConfigValue<float>(document, "effects", "saturation");
    config.effectsGamma = FindConfigValue<float>(document, "effects", "gamma");
    config.effectsTemperature = FindConfigValue<float>(document, "effects", "temperature");
    config.effectsVignette = FindConfigValue<float>(document, "effects", "vignette");
    config.reshadeEnabled = FindConfigValue<bool>(document, "reshade", "enabled");
    config.primeFastDiscReads = FindConfigValue<bool>(document, "prime", "fast_disc_reads");
    config.primeAsyncDiscReads = FindConfigValue<bool>(document, "prime", "async_disc_reads");
    config.primeGpuTextureDecode = FindConfigValue<bool>(document, "prime", "gpu_texture_decode");
    config.primeKeyboardIcons = FindConfigValue<bool>(document, "prime", "keyboard_icons");
    config.primeWiimoteBatteryFull = FindConfigValue<bool>(document, "prime", "wiimote_battery_full");
    if (const auto* keys = document.contains("prime_keys") ? &document.at("prime_keys") : nullptr;
        keys != nullptr && keys->is_table()) {
        for (const auto& [action, value] : keys->as_table()) {
            if (!value.is_array()) continue;
            auto& names = config.primeKeyBindings[action];
            for (const auto& entry : value.as_array())
                if (entry.is_string()) names.push_back(entry.as_string());
        }
    }
    if (const auto* pad = document.contains("prime_pad") ? &document.at("prime_pad") : nullptr;
        pad != nullptr && pad->is_table()) {
        for (const auto& [action, value] : pad->as_table()) {
            if (!value.is_array()) continue;
            auto& names = config.primePadBindings[action];
            for (const auto& entry : value.as_array())
                if (entry.is_string()) names.push_back(entry.as_string());
        }
    }
    if (const auto* materials = document.contains("prime_materials") ? &document.at("prime_materials") : nullptr;
        materials != nullptr && materials->is_table()) {
        for (const auto& [key, value] : materials->as_table()) {
            if (value.is_floating()) config.primeMaterials[key] = static_cast<float>(value.as_floating());
            else if (value.is_integer()) config.primeMaterials[key] = static_cast<float>(value.as_integer());
        }
    }
    if (const auto* layer = document.contains("prime_pad_motion") ? &document.at("prime_pad_motion") : nullptr;
        layer != nullptr && layer->is_table()) {
        for (const auto& [action, value] : layer->as_table()) {
            if (!value.is_array()) continue;
            auto& names = config.primePadLayerBindings[action];
            for (const auto& entry : value.as_array())
                if (entry.is_string()) names.push_back(entry.as_string());
        }
    }
    config.primeGamepad = FindConfigValue<bool>(document, "prime", "gamepad");
    config.primePadLookSpeed = FindConfigFloat(document, "prime", "pad_look_speed");
    config.primePadLookCurve = FindConfigFloat(document, "prime", "pad_look_curve");
    config.primePadPointerSpeed = FindConfigFloat(document, "prime", "pad_pointer_speed");
    config.primePadDeadzone = FindConfigFloat(document, "prime", "pad_deadzone");
    config.primePadInvertY = FindConfigValue<bool>(document, "prime", "pad_invert_y");
    config.primePadIcons = FindConfigValue<bool>(document, "prime", "pad_icons");
    config.primeFovEnabled = FindConfigValue<bool>(document, "prime", "fov_enabled");
    config.primeFov = FindConfigFloat(document, "prime", "fov");
    config.primeFovCullingOff = FindConfigValue<bool>(document, "prime", "fov_culling_off");
    config.primeFovArmAdjust = FindConfigValue<bool>(document, "prime", "fov_arm_adjust");
    config.primeAchievements = FindConfigValue<bool>(document, "prime", "achievements");
    config.primeAchievementsNotifications = FindConfigValue<bool>(document, "prime", "achievements_notifications");
    config.forceAspect169 = FindConfigValue<bool>(document, "video", "force_16_9");
    config.primeAspectRatio = FindConfigValue<std::string>(document, "video", "aspect_ratio");
    config.windowPosX = FindConfigInt(document, "video", "window_x");
    config.windowPosY = FindConfigInt(document, "video", "window_y");
    if (auto value = FindConfigUint(document, "video", "window_width"); value && *value != 0) {
        config.windowWidth = *value;
    }
    if (auto value = FindConfigUint(document, "video", "window_height"); value && *value != 0) {
        config.windowHeight = *value;
    }
    if (auto value = FindConfigFloat(document, "video", "resolution_multiplier");
        value && IsSupportedResolutionMultiplier(*value)) {
        config.resolutionMultiplier = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "graphics_api")) {
        if (IsSupportedGraphicsApi(*value)) {
            config.graphicsApi = *value;
        } else {
            std::cerr << "[runtime] Unknown video.graphics_api=\"" << *value
                      << "\", using the automatic backend" << std::endl;
        }
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "display_mode");
        value && IsSupportedDisplayMode(*value)) {
        config.displayMode = *value;
    }
    if (auto value = FindConfigUint(document, "video", "frame_interpolation_fps")) {
        if (IsSupportedFrameInterpolationFps(*value)) {
            config.frameInterpolationFps = *value;
        }
    }
    config.frameInterpolationLowLatency =
        FindConfigValue<bool>(document, "video", "frame_interpolation_low_latency");
    config.vsync = FindConfigValue<bool>(document, "video", "vsync");
    config.frameInterpolationDisplaySync =
        FindConfigValue<bool>(document, "video", "frame_interpolation_display_sync");
    config.metalFxSpatialUpscaling =
        FindConfigValue<bool>(document, "video", "metalfx_spatial_upscaling");
    config.skipUnreadyPipelines = FindConfigValue<bool>(document, "video", "skip_unready_pipelines");
    config.ubershader = FindConfigValue<bool>(document, "video", "ubershader");
    config.disableCopyFilter = FindConfigValue<bool>(document, "video", "disable_copy_filter");
    config.showFps = FindConfigValue<bool>(document, "video", "show_fps");
    config.textureReplacements = FindConfigValue<bool>(document, "video", "texture_replacements");
    config.textureDumps = FindConfigValue<bool>(document, "video", "texture_dumps");
    if (auto value = FindConfigUint(document, "video", "msaa"); value && IsSupportedMsaa(*value)) {
        config.msaa = *value;
    }
    if (auto value = FindConfigUint(document, "video", "anisotropic_filtering");
        value && IsSupportedAnisotropicFiltering(*value)) {
        config.anisotropicFiltering = *value;
    }
    config.perPixelLighting = FindConfigValue<bool>(document, "video", "per_pixel_lighting");
    config.enhancedMetal = FindConfigValue<bool>(document, "video", "enhanced_metal");
    config.enhancedMaterials = FindConfigValue<bool>(document, "video", "enhanced_materials");
    config.modernLighting = FindConfigValue<bool>(document, "video", "modern_lighting");
    config.modernEngineLights = FindConfigValue<bool>(document, "video", "engine_lights");
    config.modernExtraDynamicLights = FindConfigValue<bool>(document, "video", "extra_dynamic_lights");
    config.modernContactShadows = FindConfigValue<bool>(document, "video", "contact_shadows");
    config.modernTemporal = FindConfigValue<bool>(document, "video", "temporal_stability");
    config.modernActorLightFade = FindConfigValue<bool>(document, "video", "actor_light_fade");
    config.modernLightmapDither = FindConfigValue<bool>(document, "video", "lightmap_dither");
    config.modernAutoExposure = FindConfigValue<bool>(document, "video", "auto_exposure");
    config.modernContactStrength = FindConfigValue<float>(document, "video", "contact_shadows_strength");
    config.modernContactLength = FindConfigValue<float>(document, "video", "contact_shadows_length");
    config.modernExposureRange = FindConfigValue<float>(document, "video", "exposure_range");
    config.modernExposureSpeed = FindConfigValue<float>(document, "video", "exposure_speed");
    config.enhancedMetalStrength = FindConfigValue<float>(document, "video", "enhanced_metal_strength");
    config.enhancedMetalRoughness = FindConfigValue<float>(document, "video", "enhanced_metal_roughness");
    config.enhancedMetalMetallic = FindConfigValue<float>(document, "video", "enhanced_metal_metallic");
    config.enhancedMetalEdge = FindConfigValue<float>(document, "video", "enhanced_metal_edge");
    config.enhancedMetalDetail = FindConfigValue<float>(document, "video", "enhanced_metal_detail");
    if (auto value = FindConfigUint(document, "video", "texture_upscale");
        value && IsSupportedTextureUpscale(*value)) {
        config.textureUpscale = *value;
    }
    if (auto value = FindConfigUint(document, "video", "texture_upscale_max_size");
        value && IsSupportedTextureUpscaleMaxSize(*value)) {
        config.textureUpscaleMaxSize = *value;
    }
    if (auto value = FindConfigFloat(document, "video", "texture_upscale_sharpen");
        value && *value >= 0.0f && *value <= 1.0f) {
        config.textureUpscaleSharpen = *value;
    }
    if (auto value = FindConfigUint(document, "video", "disabled_post_processing_paths");
        value && (*value & ~0x10u) == 0) {
        config.disabledPostProcessingPaths = *value & 0x10u;
    }

    auto readVolume = [&](std::string_view key) -> std::optional<float> {
        auto value = FindConfigFloat(document, "audio", key);
        return value && *value >= 0.0f && *value <= 1.0f ? value : std::nullopt;
    };
    config.audioVolume = readVolume("volume");
    config.audioMusicVolume = readVolume("music_volume");
    config.audioSoundEffectsVolume = readVolume("sound_effects_volume");
    config.audioUiVolume = readVolume("ui_volume");
    config.audioVoicesVolume = readVolume("voices_volume");
    config.audioMuted = FindConfigValue<bool>(document, "audio", "muted");
    config.audioMixWorker = FindConfigValue<bool>(document, "audio", "mix_worker");
    config.attenuateMusicWhenMediaPlays =
        FindConfigValue<bool>(document, "audio", "attenuate_music_when_media_plays");
    config.wiiRemotes = FindConfigValue<bool>(document, "controller", "wii_remotes");
    config.wiiContinuousScan = FindConfigValue<bool>(document, "controller", "wii_continuous_scan");
    for (const char* key : {"wii_accel_offset_x", "wii_accel_offset_y", "wii_accel_offset_z"}) {
        config.hasLegacyWiiAccelOffset |= FindConfigValue<double>(document, "controller", key).value_or(0.0) != 0.0;
    }
    config.wiiAccelTrace = FindConfigValue<bool>(document, "controller", "wii_accel_trace");
    config.networkEnabled = FindConfigValue<bool>(document, "network", "enabled");
    config.discordPresenceEnabled = FindConfigValue<bool>(document, "discord", "enabled");
    config.discordClientId = FindConfigValue<std::string>(document, "discord", "client_id");

    config.nandRoot = FindConfigValue<std::string>(document, "paths", "nand_root");
    config.dvdRoot = FindConfigValue<std::string>(document, "paths", "dvd_root");
    config.primeIsoPath = FindConfigValue<std::string>(document, "paths", "iso_path");
    config.captureDir = FindConfigValue<std::string>(document, "paths", "capture_dir");
    config.retroRewindRoot = FindConfigValue<std::string>(document, "paths", "retro_rewind_root");
    if (auto roots = FindConfigValue<std::vector<std::string>>(document, "paths", "overlay_roots")) {
        for (auto& root : *roots) {
            root = Trim(root);
            if (!root.empty()) {
                config.overlayRoots.push_back(std::move(root));
            }
        }
    } else if (auto roots = FindConfigValue<std::string>(document, "paths", "overlay_roots")) {
        AppendOverlayRoots(config, *roots);
    }

    return config;
}

inline RuntimeUserConfig ParseConfig(std::istream& input, std::string sourceName = "Config.toml") {
    try {
        return ParseConfigDocument(toml::parse(input, std::move(sourceName)));
    } catch (const std::exception& exception) {
        std::cerr << "[runtime-config] Invalid TOML; using built-in defaults: "
                  << exception.what() << std::endl;
        return {};
    }
}

inline RuntimeUserConfig LoadConfigFile() {
    EnsureConfigFile();
    std::ifstream file(ResolveConfigPath(), std::ios::binary);
    return file ? ParseConfig(file, PathToUtf8(ResolveConfigPath())) : RuntimeUserConfig{};
}

inline const RuntimeUserConfig& Get() {
    static RuntimeUserConfig config = LoadConfigFile();
    return config;
}

inline RuntimeUserConfig& Mutable() {
    return const_cast<RuntimeUserConfig&>(Get());
}

inline constexpr std::array<std::string_view, 12> kControllerButtonKeys = {
    "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
};

inline const std::optional<std::string>& ControllerButton(size_t index) {
    static const std::optional<std::string> empty;
    return index < Get().controllerButtons.size() ? Get().controllerButtons[index] : empty;
}

// Sets section.key (section may be a dotted table path) to a TOML literal, keeping the rest of the
// document's keys, order and comments. Returns nullopt when the document or value does not parse.
inline std::optional<std::string> UpdateSettingText(const std::string& text, std::string_view section,
                                                    std::string_view key, std::string_view value) {
    try {
        auto root = toml::parse_str<toml::ordered_type_config>(text);
        auto literal = toml::parse_str<toml::ordered_type_config>("v = " + std::string(value));
        auto* table = &root;
        std::istringstream path{std::string(section)};
        for (std::string part; std::getline(path, part, '.');) {
            auto& parent = table->as_table();
            if (!parent.contains(part)) {
                parent[part] = toml::ordered_value(toml::ordered_table{});
            }
            table = &parent.at(part);
            if (!table->is_table()) {
                return std::nullopt;
            }
        }
        if (table->as_table_fmt().fmt == toml::table_format::implicit) {
            table->as_table_fmt().fmt = toml::table_format::multiline;
        }
        auto& slot = table->as_table()[std::string(key)];
        auto& updated = literal.as_table().at("v");
        updated.comments() = slot.comments();
        slot = std::move(updated);
        return toml::format(root);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Update one TOML value without discarding unrelated settings or user-specific paths. This is
// used by the in-game F10 settings bar. An unparseable file is left untouched.
inline bool WriteSetting(std::string_view section, std::string_view key, std::string_view value) {
    const auto path = ResolveConfigPath();
    std::ostringstream existing;
    if (std::ifstream input(path, std::ios::binary); input) {
        existing << input.rdbuf();
    }
    const auto updated = UpdateSettingText(existing.str(), section, key, value);
    if (!updated) {
        std::cerr << "[runtime-config] Not updating invalid " << PathToUtf8(path) << std::endl;
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        std::cerr << "[runtime-config] Unable to write " << PathToUtf8(path) << std::endl;
        return false;
    }
    output << *updated;
    return static_cast<bool>(output);
}

// Several keys of one section in a single read and write of the file (WriteSetting once per key
// would re-read, re-parse and rewrite the whole document each time).
inline bool WriteSettings(std::string_view section,
                          const std::vector<std::pair<std::string, std::string>>& keyValues) {
    if (keyValues.empty()) return true;
    const auto path = ResolveConfigPath();
    std::ostringstream existing;
    if (std::ifstream input(path, std::ios::binary); input) {
        existing << input.rdbuf();
    }
    std::string text = existing.str();
    try {
        auto root = toml::parse_str<toml::ordered_type_config>(text);
        auto* table = &root;
        std::istringstream sectionPath{std::string(section)};
        for (std::string part; std::getline(sectionPath, part, '.');) {
            auto& parent = table->as_table();
            if (!parent.contains(part)) parent[part] = toml::ordered_value(toml::ordered_table{});
            table = &parent.at(part);
            if (!table->is_table()) return false;
        }
        if (table->as_table_fmt().fmt == toml::table_format::implicit) {
            table->as_table_fmt().fmt = toml::table_format::multiline;
        }
        for (const auto& [key, value] : keyValues) {
            auto literal = toml::parse_str<toml::ordered_type_config>("v = " + value);
            auto& slot = table->as_table()[key];
            auto& updated = literal.as_table().at("v");
            updated.comments() = slot.comments();
            slot = std::move(updated);
        }
        text = toml::format(root);
    } catch (const std::exception&) {
        std::cerr << "[runtime-config] Not updating invalid " << PathToUtf8(path) << std::endl;
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        std::cerr << "[runtime-config] Unable to write " << PathToUtf8(path) << std::endl;
        return false;
    }
    output << text;
    return static_cast<bool>(output);
}

inline std::string FormatString(std::string_view value) {
    return toml::format(toml::value(std::string(value)));
}

inline bool SetResolutionMultiplier(float value) {
    Mutable().resolutionMultiplier = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("video", "resolution_multiplier", formatted.str());
}

inline bool SetWindowSize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    Mutable().windowWidth = width;
    Mutable().windowHeight = height;
    const bool wroteWidth = WriteSetting("video", "window_width", std::to_string(width));
    const bool wroteHeight = WriteSetting("video", "window_height", std::to_string(height));
    return wroteWidth && wroteHeight;
}

inline bool SetWindowPosition(int32_t x, int32_t y) {
    Mutable().windowPosX = x;
    Mutable().windowPosY = y;
    const bool wroteX = WriteSetting("video", "window_x", std::to_string(x));
    const bool wroteY = WriteSetting("video", "window_y", std::to_string(y));
    return wroteX && wroteY;
}

inline bool SetFrameInterpolationFps(uint32_t value) {
    if (!IsSupportedFrameInterpolationFps(value)) {
        return false;
    }
    Mutable().frameInterpolationFps = value;
    return WriteSetting("video", "frame_interpolation_fps", std::to_string(value));
}

inline bool SetVsync(bool value) {
    Mutable().vsync = value;
    return WriteSetting("video", "vsync", value ? "true" : "false");
}
inline bool SetFrameInterpolationLowLatency(bool value) {
    Mutable().frameInterpolationLowLatency = value;
    return WriteSetting("video", "frame_interpolation_low_latency", value ? "true" : "false");
}

inline bool SetFrameInterpolationDisplaySync(bool value) {
    Mutable().frameInterpolationDisplaySync = value;
    return WriteSetting("video", "frame_interpolation_display_sync", value ? "true" : "false");
}

inline bool SetMetalFxSpatialUpscaling(bool value) {
    Mutable().metalFxSpatialUpscaling = value;
    return WriteSetting("video", "metalfx_spatial_upscaling", value ? "true" : "false");
}

inline bool SetDisplayMode(std::string value) {
    if (!IsSupportedDisplayMode(value)) {
        return false;
    }
    Mutable().displayMode = value;
    return WriteSetting("video", "display_mode", FormatString(value));
}

// [effects] and [reshade] setters (F1 > Effects).
inline bool SetEffectsOption(std::optional<bool> RuntimeUserConfig::*field, const char* key, bool value) {
    Mutable().*field = value;
    return WriteSetting("effects", key, value ? "true" : "false");
}
inline bool SetEffectsFloat(std::optional<float> RuntimeUserConfig::*field, const char* key, float value) {
    Mutable().*field = value;
    std::ostringstream formatted;
    formatted << value;
    std::string text = formatted.str();
    // Keep it a TOML float: "8" would be read back as an integer.
    if (text.find_first_of(".eEni") == std::string::npos) text += ".0";
    return WriteSetting("effects", key, text);
}
inline bool ReshadeEnabled() { return Get().reshadeEnabled.value_or(false); }
inline bool SetReshadeEnabled(bool value) {
    Mutable().reshadeEnabled = value;
    return WriteSetting("reshade", "enabled", value ? "true" : "false");
}
// [paths] iso_path, written once the chosen disc image has opened. Refuses
// shell locations ("::{...}"), which name no file (discussion #4).
inline bool SetPrimeIsoPath(const std::filesystem::path& value) {
    const std::string utf8 = PathToUtf8(value);
    if (PrimeDiscPath::CheckText(utf8) != PrimeDiscPath::Problem::None) {
        return false;
    }
    Mutable().primeIsoPath = utf8;
    return WriteSetting("paths", "iso_path", FormatString(utf8));
}
inline bool SetGraphicsApi(std::string value) {
    if (!IsSupportedGraphicsApi(value)) {
        return false;
    }
    Mutable().graphicsApi = value;
    return WriteSetting("video", "graphics_api", FormatString(value));
}
inline bool SetUbershader(bool value) {
    Mutable().ubershader = value;
    return WriteSetting("video", "ubershader", value ? "true" : "false");
}
inline bool SetSkipUnreadyPipelines(bool value) {
    Mutable().skipUnreadyPipelines = value;
    return WriteSetting("video", "skip_unready_pipelines", value ? "true" : "false");
}

inline bool SetDisableCopyFilter(bool value) {
    Mutable().disableCopyFilter = value;
    return WriteSetting("video", "disable_copy_filter", value ? "true" : "false");
}

inline bool SetShowFps(bool value) {
    Mutable().showFps = value;
    return WriteSetting("video", "show_fps", value ? "true" : "false");
}

inline bool SetMsaa(uint32_t value) {
    if (!IsSupportedMsaa(value)) return false;
    Mutable().msaa = value;
    return WriteSetting("video", "msaa", std::to_string(value));
}

inline bool SetAnisotropicFiltering(uint32_t value) {
    if (!IsSupportedAnisotropicFiltering(value)) return false;
    Mutable().anisotropicFiltering = value;
    return WriteSetting("video", "anisotropic_filtering", std::to_string(value));
}

inline bool SetPerPixelLighting(bool value) {
    Mutable().perPixelLighting = value;
    return WriteSetting("video", "per_pixel_lighting", value ? "true" : "false");
}

inline bool SetEnhancedMaterials(bool value) {
    Mutable().enhancedMaterials = value;
    return WriteSetting("video", "enhanced_materials", value ? "true" : "false");
}

inline bool SetEnhancedMetal(bool value) {
    Mutable().enhancedMetal = value;
    return WriteSetting("video", "enhanced_metal", value ? "true" : "false");
}

inline bool SetVideoOption(std::optional<bool> RuntimeUserConfig::*field, const char* key, bool value) {
    Mutable().*field = value;
    return WriteSetting("video", key, value ? "true" : "false");
}

inline bool SetVideoFloat(std::optional<float> RuntimeUserConfig::*field, const char* key, float value) {
    Mutable().*field = value;
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return WriteSetting("video", key, text);
}

inline bool SetEnhancedMetalStrength(float value) {
    Mutable().enhancedMetalStrength = value;
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return WriteSetting("video", "enhanced_metal_strength", text);
}

inline bool SetTextureUpscale(uint32_t value) {
    if (!IsSupportedTextureUpscale(value)) return false;
    Mutable().textureUpscale = value;
    return WriteSetting("video", "texture_upscale", std::to_string(value));
}

inline bool SetTextureUpscaleSharpen(float value) {
    if (!(value >= 0.0f && value <= 1.0f)) return false;
    Mutable().textureUpscaleSharpen = value;
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(2) << value;
    return WriteSetting("video", "texture_upscale_sharpen", formatted.str());
}

inline bool SetTextureUpscaleMaxSize(uint32_t value) {
    if (!IsSupportedTextureUpscaleMaxSize(value)) return false;
    Mutable().textureUpscaleMaxSize = value;
    return WriteSetting("video", "texture_upscale_max_size", std::to_string(value));
}

inline bool SetTextureReplacements(bool value) {
    Mutable().textureReplacements = value;
    return WriteSetting("video", "texture_replacements", value ? "true" : "false");
}

inline bool SetTextureDumps(bool value) {
    Mutable().textureDumps = value;
    return WriteSetting("video", "texture_dumps", value ? "true" : "false");
}

inline bool SetDisabledPostProcessingPaths(uint32_t value) {
    Mutable().disabledPostProcessingPaths = value;
    std::ostringstream formatted;
    formatted << "0x" << std::hex << std::uppercase << value;
    return WriteSetting("video", "disabled_post_processing_paths", formatted.str());
}

inline bool SetControllerButton(size_t index, std::string value) {
    if (index >= kControllerButtonKeys.size()) {
        return false;
    }
    Mutable().controllerButtons[index] = value;
    return WriteSetting("controller", kControllerButtonKeys[index], FormatString(value));
}

inline std::string ControllerExpression(const std::string& key) {
    const auto it = Get().controllerExpressions.find(key);
    return it == Get().controllerExpressions.end() ? std::string() : it->second;
}

inline bool SetControllerExpression(const std::string& key, const std::string& value) {
    Mutable().controllerExpressions[key] = value;
    return WriteSetting("controller", key, FormatString(value));
}

inline bool RumbleEnabled(bool fallback = true) {
    return Get().rumbleEnabled.value_or(fallback);
}

inline bool SetRumbleEnabled(bool value) {
    Mutable().rumbleEnabled = value;
    return WriteSetting("controller", "rumble", value ? "true" : "false");
}

inline int32_t MuteHotkey(int32_t fallback) {
    return Get().muteHotkey.value_or(fallback);
}

inline bool SetMuteHotkey(int32_t value) {
    Mutable().muteHotkey = value;
    return WriteSetting("audio", "mute_key", std::to_string(value));
}

inline bool SetAudioVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "volume", formatted.str());
}

inline bool SetMusicVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioMusicVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "music_volume", formatted.str());
}

inline bool SetSoundEffectsVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioSoundEffectsVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "sound_effects_volume", formatted.str());
}

inline bool SetUiVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioUiVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "ui_volume", formatted.str());
}

inline bool SetVoicesVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVoicesVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "voices_volume", formatted.str());
}

inline bool SetAudioMuted(bool value) {
    Mutable().audioMuted = value;
    return WriteSetting("audio", "muted", value ? "true" : "false");
}

inline bool SetAudioMixWorker(bool value) {
    Mutable().audioMixWorker = value;
    return WriteSetting("audio", "mix_worker", value ? "true" : "false");
}

inline bool SetAttenuateMusicWhenMediaPlays(bool value) {
    Mutable().attenuateMusicWhenMediaPlays = value;
    return WriteSetting("audio", "attenuate_music_when_media_plays", value ? "true" : "false");
}

inline std::string PrimeSoundMode() { return Get().primeSoundMode.value_or("stereo"); }
// The console language the game reads (SYSCONF IPL.LNG); see prime_host_options.cpp.
inline std::string PrimeLanguage() { return Get().primeLanguage.value_or("english"); }
inline bool SetPrimeLanguage(const std::string& value) {
    Mutable().primeLanguage = value;
    return WriteSetting("prime", "language", FormatString(value));
}
inline bool SetPrimeSoundMode(const std::string& value) {
    Mutable().primeSoundMode = value;
    return WriteSetting("prime", "sound_mode", FormatString(value));
}
// Defaults are the tuned values; an environment variable set to 0/1 still overrides them.
inline bool PrimeOption(std::optional<bool> RuntimeUserConfig::*field, const char* environment, bool fallback) {
    if (const char* value = std::getenv(environment); value && (value[0] == '0' || value[0] == '1') && !value[1])
        return value[0] == '1';
    return (Get().*field).value_or(fallback);
}
inline bool SetPrimeOption(std::optional<bool> RuntimeUserConfig::*field, const char* key, bool value) {
    Mutable().*field = value;
    return WriteSetting("prime", key, value ? "true" : "false");
}
inline bool SetPrimeFloatOption(std::optional<float> RuntimeUserConfig::*field, const char* key, float value) {
    Mutable().*field = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("prime", key, formatted.str());
}

inline bool WidescreenEnabled(bool fallback = false) {
    return Get().widescreen.value_or(fallback);
}

inline bool SetWidescreen(bool value) {
    Mutable().widescreen = value;
    return WriteSetting("video", "widescreen", value ? "true" : "false");
}
inline bool ForceAspect169Enabled(bool fallback = false) {
    return Get().forceAspect169.value_or(fallback);
}

// [video] aspect_ratio (hle/prime_widescreen.h): the shape of the 16:9 mode's image.
inline bool SetPrimeAspectRatio(const std::string& value) {
    Mutable().primeAspectRatio = value;
    return WriteSetting("video", "aspect_ratio", FormatString(value));
}

inline bool SetForceAspect169(bool value) {
    Mutable().forceAspect169 = value;
    return WriteSetting("video", "force_16_9", value ? "true" : "false");
}

inline bool WindowPosition(int32_t& x, int32_t& y) {
    if (!Get().windowPosX || !Get().windowPosY) {
        return false;
    }
    x = *Get().windowPosX;
    y = *Get().windowPosY;
    return true;
}

inline uint32_t WindowWidth(uint32_t fallback) {
    return Get().windowWidth.value_or(fallback);
}

inline uint32_t WindowHeight(uint32_t fallback) {
    return Get().windowHeight.value_or(fallback);
}

inline float ResolutionMultiplier(float fallback = 1.0f) {
    return std::max(0.0f, Get().resolutionMultiplier.value_or(fallback));
}

inline bool MetalFxSpatialUpscaling(bool fallback = false) {
    return Get().metalFxSpatialUpscaling.value_or(fallback);
}

inline float AudioVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float MusicVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioMusicVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float SoundEffectsVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioSoundEffectsVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float UiVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioUiVolume.value_or(fallback), 0.0f, 1.0f);
}

inline float VoicesVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVoicesVolume.value_or(fallback), 0.0f, 1.0f);
}

inline bool AudioMuted(bool fallback = false) {
    return Get().audioMuted.value_or(fallback);
}

// Off-thread AX/DSP mix. Default on; false restores the fully synchronous mix.
inline bool AudioMixWorkerEnabled(bool fallback = true) {
    return Get().audioMixWorker.value_or(fallback);
}

// Whether background music should duck automatically for other media playback.
inline bool AttenuateMusicWhenMediaPlays(bool fallback = false) {
    return Get().attenuateMusicWhenMediaPlays.value_or(fallback);
}

// Bluetooth Wii Remotes / Wii U Pro Controllers. Read once before SDL's joystick
// subsystem comes up, so a change only takes effect on the next launch.
inline bool WiiRemotesEnabled(bool fallback = true) {
    return Get().wiiRemotes.value_or(fallback);
}

// Persists the Bluetooth Wii Remote driver switch.
inline bool SetWiiRemotesEnabled(bool value) {
    Mutable().wiiRemotes = value;
    return WriteSetting("controller", "wii_remotes", value ? "true" : "false");
}

// Whether to keep rescanning Bluetooth while no Wii controller is connected.
inline bool WiiContinuousScanEnabled(bool fallback = false) {
    return Get().wiiContinuousScan.value_or(fallback);
}

// Persists the continuous scanning switch.
inline bool SetWiiContinuousScanEnabled(bool value) {
    Mutable().wiiContinuousScan = value;
    return WriteSetting("controller", "wii_continuous_scan", value ? "true" : "false");
}

// Stored correction of the remote identified by `remoteKey`, if it has one.
inline std::optional<std::array<double, 3>> WiiAccelOffsetFor(const std::string& remoteKey) {
    const auto it = Get().wiiAccelOffsets.find(remoteKey);
    if (it == Get().wiiAccelOffsets.end()) {
        return std::nullopt;
    }
    return it->second;
}

// Whether to write the per-frame accelerometer trace (off unless asked for).
inline bool WiiAccelTraceEnabled(bool fallback = false) {
    return Get().wiiAccelTrace.value_or(fallback);
}

// Persists the accelerometer correction of one remote. `remoteKey` must be a bare TOML key.
inline bool SetWiiAccelOffsetFor(const std::string& remoteKey, const std::array<double, 3>& offset) {
    Mutable().wiiAccelOffsets[remoteKey] = offset;
    // Always float literals, so a whole-number offset does not come back as a TOML integer.
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(4) << '[' << offset[0] << ", " << offset[1] << ", " << offset[2]
              << ']';
    return WriteSetting("controller.wii_accel_offsets", remoteKey, formatted.str());
}

// Target frame rate for frame interpolation, or 0 to disable it.
inline uint32_t FrameInterpolationFps(uint32_t fallback = 0) {
    return Get().frameInterpolationFps.value_or(fallback);
}

inline bool FrameInterpolationDisplaySync(bool fallback = true) {
    return Get().frameInterpolationDisplaySync.value_or(fallback);
}

inline bool Vsync(bool fallback = false) { return Get().vsync.value_or(fallback); }
inline bool FrameInterpolationLowLatency(bool fallback = false) {
    return Get().frameInterpolationLowLatency.value_or(fallback);
}

// Whether to skip draws whose graphics pipeline has not finished compiling yet.
inline bool Ubershader(bool fallback = true) {
    // WII_COMPILED_UBERSHADER=1/0 overrides the setting (A/B checks without touching Config.toml).
    if (const char* value = std::getenv("WII_COMPILED_UBERSHADER")) return value[0] != '0';
    return Get().ubershader.value_or(fallback);
}
inline bool SkipUnreadyPipelines(bool fallback = true) {
    return Get().skipUnreadyPipelines.value_or(fallback);
}

inline bool DisableCopyFilter(bool fallback = true) {
    return Get().disableCopyFilter.value_or(fallback);
}

inline bool ShowFps(bool fallback = true) {
    return Get().showFps.value_or(fallback);
}

inline bool TextureReplacements(bool fallback = false) {
    return Get().textureReplacements.value_or(fallback);
}

// Dumping only produces the names a replacement would need, so main.cpp
// gates it on TextureReplacements() as well.
inline bool TextureDumps(bool fallback = false) {
    return Get().textureDumps.value_or(fallback);
}

inline uint32_t Msaa(uint32_t fallback = 1) {
    return Get().msaa.value_or(fallback);
}

inline uint32_t AnisotropicFiltering(uint32_t fallback = 0) {
    return Get().anisotropicFiltering.value_or(fallback);
}

inline bool PerPixelLighting(bool fallback = false) {
    return Get().perPixelLighting.value_or(fallback);
}

inline bool EnhancedMaterials(bool fallback = true) {
    return Get().enhancedMaterials.value_or(fallback);
}

inline bool EnhancedMetal(bool fallback = false) {
    return Get().enhancedMetal.value_or(fallback);
}

inline float EnhancedMetalStrength(float fallback = 1.0f) {
    return Get().enhancedMetalStrength.value_or(fallback);
}

inline uint32_t TextureUpscale(uint32_t fallback = 0) {
    return Get().textureUpscale.value_or(fallback);
}

inline uint32_t TextureUpscaleMaxSize(uint32_t fallback = 256) {
    return Get().textureUpscaleMaxSize.value_or(fallback);
}

inline float TextureUpscaleSharpen(float fallback = 0.0f) {
    return Get().textureUpscaleSharpen.value_or(fallback);
}

inline uint32_t DisabledPostProcessingPaths(uint32_t fallback = 0) {
    return Get().disabledPostProcessingPaths.value_or(fallback) & 0x10u;
}

inline std::string GraphicsApi(std::string fallback = "auto") {
    return Get().graphicsApi.value_or(std::move(fallback));
}

inline std::string DisplayMode(std::string fallback = "windowed") {
    return Get().displayMode.value_or(std::move(fallback));
}

inline bool NetworkEnabled(bool fallback = true) {
    return Get().networkEnabled.value_or(fallback);
}

inline std::string NandRoot(std::string fallback = "") {
    return Get().nandRoot.value_or(std::move(fallback));
}

inline std::string DvdRoot(std::string fallback = "") {
    return Get().dvdRoot.value_or(std::move(fallback));
}

// The one resolver for configured paths. A relative value means the same thing
// everywhere it can be configured: relative to the config file that named it,
// never to the process working directory (docs/WHEELWIZARD_CONTRACT.md).
inline std::filesystem::path ResolveRelativeTo(const std::filesystem::path& base,
                                               const std::string& value) {
    std::filesystem::path path = PathFromUtf8(value);
    if (path.is_relative()) {
        path = base / path;
    }
    return path.lexically_normal();
}

inline std::filesystem::path ResolveRelativeToConfig(const std::string& value) {
    return ResolveRelativeTo(ResolveConfigPath().parent_path(), value);
}

// The extracted DATA directory. Empty when nothing is configured.
inline std::filesystem::path ResolvedDvdRoot() {
    const std::string configured = DvdRoot();
    return configured.empty() ? std::filesystem::path{} : ResolveRelativeToConfig(configured);
}

// `<portable root>/game` (or `<executable dir>/game`): players drop their own
// Metroid Prime 3 disc image here instead of editing Config.toml.
inline std::filesystem::path PrimeGameImageDirectory() {
    if (const auto& portableRoot = PortableRootDirectory()) {
        return *portableRoot / "game";
    }
    if (const auto executableDirectory = ExecutableDirectory()) {
        return *executableDirectory / "game";
    }
    return std::filesystem::path{"game"};
}

inline bool IsSupportedDiscImageExtension(std::filesystem::path extension) {
    std::string text = extension.string();
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text == ".iso" || text == ".rvz" || text == ".wbfs" || text == ".gcz" || text == ".wia" || text == ".ciso";
}

// [paths] capture_dir when set (relative to the config file), else UserData/Logs/captures.
inline std::filesystem::path DrawCaptureDirectory() {
    const auto configured = Get().captureDir.value_or("");
    if (!configured.empty()) return ResolveRelativeToConfig(configured);
    return ApplicationDataDirectory() / "Logs" / "captures";
}

// Disc image chosen outside Config.toml: `--iso <path>` or the CORRUPTION3_ISO
// environment variable (discussion #5). Read once; never written to the file.
// Cleared when the player picks another image after it failed to open.
inline std::string& PrimeIsoOverrideStorage() {
    static std::string value = []() -> std::string {
#if defined(_WIN32)
        const wchar_t* wide = ::_wgetenv(L"CORRUPTION3_ISO");
        return wide ? PrimeDiscPath::Normalize(PathToUtf8(std::filesystem::path(wide))) : std::string{};
#else
        const char* text = std::getenv(PrimeDiscPath::kIsoEnvironmentVariable);
        return text ? PrimeDiscPath::Normalize(text) : std::string{};
#endif
    }();
    return value;
}
inline const std::string& PrimeIsoOverride() { return PrimeIsoOverrideStorage(); }
// Also exported to the environment so OSRestart relaunches and the Trilogy
// executables open the same image.
inline void SetPrimeIsoOverride(std::string_view utf8) {
    std::string value = PrimeDiscPath::Normalize(utf8);
    // Relative to the working directory of the command line that typed it;
    // made absolute now so relaunches resolve it the same way.
    if (!PrimeDiscPath::IsShellLocation(value) && !value.empty() && PathFromUtf8(value).is_relative()) {
        std::error_code ec;
        if (auto absolute = std::filesystem::absolute(PathFromUtf8(value), ec); !ec) value = PathToUtf8(absolute.lexically_normal());
    }
    PrimeIsoOverrideStorage() = std::move(value);
#if defined(_WIN32)
    ::_wputenv_s(L"CORRUPTION3_ISO", PathFromUtf8(PrimeIsoOverrideStorage()).wstring().c_str());
#else
    ::setenv(PrimeDiscPath::kIsoEnvironmentVariable, PrimeIsoOverrideStorage().c_str(), 1);
#endif
}
inline void ClearPrimeIsoOverride() {
    if (PrimeIsoOverrideStorage().empty()) return;
    PrimeIsoOverrideStorage().clear();
#if defined(_WIN32)
    ::_wputenv_s(L"CORRUPTION3_ISO", L"");
#else
    ::unsetenv(PrimeDiscPath::kIsoEnvironmentVariable);
#endif
}
inline bool PrimeDiscDialogDisabled() {
    const char* text = std::getenv(PrimeDiscPath::kNoDialogEnvironmentVariable);
    return text && text[0] == '1' && !text[1];
}

// Where ResolvedPrimeIsoPath took its answer from.
using PrimeIsoSource = PrimeDiscPath::Source;
inline const char* PrimeIsoSourceName(PrimeIsoSource source) { return PrimeDiscPath::SourceName(source); }

// First disc image in the game folder (alphabetical, so a tester with several
// dumps gets a predictable one). Empty when there is none.
inline std::filesystem::path DetectedPrimeGameFolderImage() {
    static const std::filesystem::path detected = []() -> std::filesystem::path {
        std::error_code ec;
        std::vector<std::filesystem::path> candidates;
        for (const auto& entry : std::filesystem::directory_iterator(
                 PrimeGameImageDirectory(), std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            std::error_code entryEc;
            if (entry.is_regular_file(entryEc) && !entryEc && IsSupportedDiscImageExtension(entry.path().extension())) {
                candidates.push_back(entry.path());
            }
        }
        std::sort(candidates.begin(), candidates.end());
        return candidates.empty() ? std::filesystem::path{} : candidates.front();
    }();
    return detected;
}

// Order: --iso / CORRUPTION3_ISO, then [paths] iso_path, then the game folder
// (PrimeDiscPath::Choose, covered by mkw_prime_disc_path_tests).
inline std::filesystem::path ResolvedPrimeIsoPath(PrimeIsoSource* source = nullptr) {
    std::filesystem::path overridePath;
    if (const std::string& text = PrimeIsoOverride(); !text.empty()) {
        // Typed on a command line: relative to the working directory.
        overridePath = PathFromUtf8(text);
        if (!PrimeDiscPath::IsShellLocation(text) && overridePath.is_relative()) {
            std::error_code ec;
            if (auto absolute = std::filesystem::absolute(overridePath, ec); !ec) overridePath = std::move(absolute);
        }
        overridePath = overridePath.lexically_normal();
    }
    const std::string configured = PrimeDiscPath::Normalize(Get().primeIsoPath.value_or(""));
    std::filesystem::path configuredPath;
    if (!configured.empty()) {
        // A shell display name is not a relative path: keep it as written.
        configuredPath = PrimeDiscPath::IsShellLocation(configured) ? PathFromUtf8(configured) : ResolveRelativeToConfig(configured);
    }
    auto choice = PrimeDiscPath::Choose(overridePath, configuredPath, configured, [] { return DetectedPrimeGameFolderImage(); });
    if (source) *source = choice.source;
    return std::move(choice.path);
}

// Keyboard/mouse mode drives a virtual Wii Remote + Nunchuk. Default on: a
// player without a Bluetooth remote can start playing right away. The
// environment variable (used by the test launchers) still overrides the file.
inline bool PrimeKeyboardMouseMode() {
    static const bool enabled = [] {
        if (const char* value = std::getenv("WII_COMPILED_PRIME_KEYBOARD_MOUSE"); value && (value[0] == '0' || value[0] == '1') && !value[1])
            return value[0] == '1';
        return Get().primeKeyboardMouse.value_or(true);
    }();
    return enabled;
}

/// The canonical Retro Rewind installation the frontend owns, or "" when none is recorded.
inline std::string RetroRewindRoot(std::string fallback = "") {
    return Get().retroRewindRoot.value_or(std::move(fallback));
}

inline bool DiscordPresenceEnabled(bool fallback = true) {
    return Get().discordPresenceEnabled.value_or(fallback);
}

inline std::string DiscordClientId(std::string fallback = "1543984562369990706") {
    return Get().discordClientId.value_or(std::move(fallback));
}

inline const std::vector<std::string>& OverlayRoots() {
    return Get().overlayRoots;
}

inline void LogLoadedConfig() {
    static const bool logged = [] {
        const auto& config = Get();
        const auto configPath = ResolveConfigPath();
        std::cout << "[runtime-config] " << PathToUtf8(configPath);
        if (!std::filesystem::exists(configPath)) {
            std::cout << " not found; using built-in defaults";
        } else {
            std::cout << " loaded";
            if (config.widescreen) {
                std::cout << " widescreen=" << (*config.widescreen ? "true" : "false");
            }
            if (config.windowWidth || config.windowHeight) {
                std::cout << " window=" << config.windowWidth.value_or(0) << "x"
                          << config.windowHeight.value_or(0);
            }
            if (config.resolutionMultiplier) {
                std::cout << " resolution_multiplier=" << *config.resolutionMultiplier;
            }
            if (config.dvdRoot) {
                std::cout << " dvd_root=" << *config.dvdRoot;
            }
            if (config.graphicsApi) {
                std::cout << " graphics_api=" << *config.graphicsApi;
            }
            if (config.frameInterpolationFps) {
                std::cout << " frame_interpolation_fps=" << *config.frameInterpolationFps;
            }
            if (config.skipUnreadyPipelines) {
                std::cout << " skip_unready_pipelines=" << (*config.skipUnreadyPipelines ? "true" : "false");
            }
            if (config.disableCopyFilter) {
                std::cout << " disable_copy_filter=" << (*config.disableCopyFilter ? "true" : "false");
            }
            if (config.showFps) {
                std::cout << " show_fps=" << (*config.showFps ? "true" : "false");
            }
            if (config.textureReplacements) {
                std::cout << " texture_replacements=" << (*config.textureReplacements ? "true" : "false");
            }
            if (config.textureDumps) {
                std::cout << " texture_dumps=" << (*config.textureDumps ? "true" : "false");
            }
            if (config.audioVolume) {
                std::cout << " audio_volume=" << *config.audioVolume;
            }
            if (config.audioMuted) {
                std::cout << " audio_muted=" << (*config.audioMuted ? "true" : "false");
            }
            if (config.networkEnabled) {
                std::cout << " network_enabled=" << (*config.networkEnabled ? "true" : "false");
            }
            if (config.discordPresenceEnabled) {
                std::cout << " discord_enabled=" << (*config.discordPresenceEnabled ? "true" : "false");
            }
            if (config.nandRoot) {
                std::cout << " nand_root=" << *config.nandRoot;
            }
            if (config.retroRewindRoot) {
                std::cout << " retro_rewind_root=" << *config.retroRewindRoot;
            }
        }
        std::cout << std::endl;
        return true;
    }();
    (void)logged;
}

} // namespace RuntimeConfigFile
