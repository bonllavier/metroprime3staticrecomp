// RetroAchievements overlay for RM3E01 (hle/prime_achievements_ui.h).
#if defined(MKW_GAME_PROFILE_RM3E01_00)
#include "hle/prime_achievements_ui.h"

#include "hle/prime_achievements.h"

#include <aurora/imgui.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace PrimeAchievementsUi {
namespace {
using PrimeAchievements::LoginState;
using PrimeAchievements::GameState;
using PrimeAchievements::NoticeKind;

double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// --- images ---------------------------------------------------------------
struct RaTexture {
    ImTextureID id = 0;
    bool requested = false;
};
std::unordered_map<std::string, RaTexture> g_raTextures;

// Overlay thread (ImGui frame): uploads what the image worker decoded.
void PumpImages() {
    for (auto& image : PrimeAchievements::TakeImages()) {
        auto& texture = g_raTextures[image.url];
        if (image.width && image.height) texture.id = aurora_imgui_add_texture(image.width, image.height, image.rgba.data());
    }
}
ImTextureID TextureFor(const std::string& url) {
    if (url.empty()) return 0;
    auto& texture = g_raTextures[url];
    if (!texture.requested) {
        texture.requested = true;
        PrimeAchievements::RequestImage(url);
    }
    return texture.id;
}

// --- HUD drawing helpers (Prime 3 visor style: cyan frames, chamfered corners,
// scan lines, Phazon-orange accents) ---------------------------------------
ImU32 Col(int r, int g, int b, float a) { return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f)); }
constexpr int kCyanR = 96, kCyanG = 214, kCyanB = 255;

void Chamfer(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut) {
    dl->PathLineTo({a.x + cut, a.y});
    dl->PathLineTo({b.x, a.y});
    dl->PathLineTo({b.x, b.y - cut});
    dl->PathLineTo({b.x - cut, b.y});
    dl->PathLineTo({a.x, b.y});
    dl->PathLineTo({a.x, a.y + cut});
}

void HudPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, float s, float k, float flash, bool accent) {
    const float cut = 12.0f * s;
    for (int i = 2; i >= 1; --i) {  // glow
        const float g = 2.0f * s * static_cast<float>(i);
        Chamfer(dl, {a.x - g, a.y - g}, {b.x + g, b.y + g}, cut + g * 0.5f);
        dl->PathStroke(Col(kCyanR, kCyanG, kCyanB, k * (0.07f + 0.2f * flash)), ImDrawFlags_Closed, 1.8f * s);
    }
    Chamfer(dl, a, b, cut);
    dl->PathFillConvex(Col(5, 16, 28, k * 0.84f));
    for (float y = a.y + 3.0f * s; y < b.y - 2.0f * s; y += 3.0f * s)  // scan lines
        dl->AddLine({a.x + 2.0f * s, y}, {b.x - 2.0f * s, y}, Col(kCyanR, kCyanG, kCyanB, k * 0.035f), 1.0f);
    Chamfer(dl, a, b, cut);
    dl->PathStroke(Col(kCyanR, kCyanG, kCyanB, k * (0.7f + 0.3f * flash)), ImDrawFlags_Closed, 1.4f * s);
    // Bright segment on the top edge and an orange tick on the cut corner.
    dl->AddLine({a.x + cut + 8.0f * s, a.y}, {a.x + (b.x - a.x) * 0.55f, a.y}, Col(200, 245, 255, k * 0.9f), 2.0f * s);
    if (accent) dl->AddLine({b.x - cut * 0.9f, b.y + 1.0f * s}, {b.x + 1.0f * s, b.y - cut * 0.9f}, Col(255, 150, 60, k), 2.5f * s);
}

// Badge in a cyan frame; while the image downloads, a dim diamond.
void HudBadge(ImDrawList* dl, ImVec2 a, float size, ImTextureID texture, float k, float s, bool dim = false) {
    const ImVec2 b{a.x + size, a.y + size};
    dl->AddRectFilled(a, b, Col(10, 30, 46, k * 0.9f));
    if (texture) {
        dl->AddImage(texture, a, b, {0, 0}, {1, 1}, dim ? Col(150, 150, 150, k * 0.75f) : Col(255, 255, 255, k));
    } else {
        const ImVec2 c{a.x + size * 0.5f, a.y + size * 0.5f};
        const float r = size * 0.28f;
        dl->AddQuadFilled({c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}, Col(kCyanR, kCyanG, kCyanB, k * 0.45f));
    }
    dl->AddRect(a, b, Col(kCyanR, kCyanG, kCyanB, k * 0.85f), 0.0f, 0, 1.3f * s);
    const float t = size * 0.22f;  // corner brackets
    for (const ImVec2 corner : {a, ImVec2{b.x, a.y}, ImVec2{a.x, b.y}, b}) {
        const float dx = corner.x == a.x ? t : -t, dy = corner.y == a.y ? t : -t;
        const ImVec2 o{corner.x - (dx > 0 ? 3.0f : -3.0f) * s, corner.y - (dy > 0 ? 3.0f : -3.0f) * s};
        dl->AddLine(o, {o.x + dx, o.y}, Col(200, 245, 255, k * 0.9f), 1.6f * s);
        dl->AddLine(o, {o.x, o.y + dy}, Col(200, 245, 255, k * 0.9f), 1.6f * s);
    }
}

// Text clipped to a width with an ellipsis; up to `lines` lines, word-wrapped.
float HudText(ImDrawList* dl, ImFont* font, float size, ImVec2 at, float width, ImU32 color, const std::string& text, int lines) {
    const char* p = text.c_str();
    const char* end = p + text.size();
    float y = at.y;
    for (int line = 0; line < lines && p < end; ++line) {
        const char* wrap = font->CalcWordWrapPositionA(size / font->FontSize, p, end, width);
        if (wrap == p) wrap = p + 1;
        std::string piece(p, wrap);
        if (line == lines - 1 && wrap < end) {
            while (!piece.empty() && font->CalcTextSizeA(size, FLT_MAX, 0.0f, (piece + "...").c_str()).x > width) piece.pop_back();
            piece += "...";
        }
        dl->AddText(font, size, {at.x, y}, color, piece.c_str());
        y += size * 1.12f;
        p = wrap;
        while (p < end && *p == ' ') ++p;
    }
    return y - at.y;
}

// --- pop-ups ----------------------------------------------------------------
struct Popup {
    PrimeAchievements::Notice notice;
    double start = -1;  // shown since (seconds); -1 while queued
};
std::deque<Popup> g_raPopups;

double PopupDuration(NoticeKind kind) {
    switch (kind) {
    case NoticeKind::Completed: return 9.0;
    case NoticeKind::Unlocked: return 6.5;
    case NoticeKind::GameLoaded: return 5.5;
    default: return 5.0;
    }
}

const char* PopupHeader(NoticeKind kind) {
    switch (kind) {
    case NoticeKind::Unlocked: return "ACHIEVEMENT UNLOCKED";
    case NoticeKind::Completed: return "SET MASTERED";
    case NoticeKind::GameLoaded: return "RETROACHIEVEMENTS \xC2\xB7 CASUAL";
    case NoticeKind::Problem: return "RETROACHIEVEMENTS \xC2\xB7 WARNING";
    default: return "RETROACHIEVEMENTS";
    }
}

std::string PopupBody(const PrimeAchievements::Notice& notice) {
    if (notice.kind == NoticeKind::GameLoaded)
        return notice.text + " achievements unlocked";
    if (notice.kind == NoticeKind::Completed)
        return "Every achievement of this set is unlocked.";
    return notice.text;
}

void DrawPopups(ImFont* font, double now) {
    for (auto& notice : PrimeAchievements::TakeNotices()) g_raPopups.push_back({std::move(notice), -1});
    // Up to three at once, newest below; the rest wait their turn.
    while (!g_raPopups.empty() && g_raPopups.front().start >= 0 &&
           now - g_raPopups.front().start > PopupDuration(g_raPopups.front().notice.kind))
        g_raPopups.pop_front();
    int shown = 0;
    for (auto& popup : g_raPopups) {
        if (shown == 3) break;
        if (popup.start < 0) popup.start = now;
        ++shown;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float s = std::clamp(display.y / 1080.0f, 0.6f, 2.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float width = 470.0f * s, height = 104.0f * s, margin = 26.0f * s, gap = 12.0f * s;
    float y = display.y * 0.17f;
    int index = 0;
    for (const auto& popup : g_raPopups) {
        if (index++ == 3 || popup.start < 0) break;
        const auto& notice = popup.notice;
        const double age = now - popup.start, duration = PopupDuration(notice.kind);
        const float in = static_cast<float>(std::clamp(age / 0.35, 0.0, 1.0));
        const float out = static_cast<float>(std::clamp((duration - age) / 0.5, 0.0, 1.0));
        const float ease = 1.0f - (1.0f - in) * (1.0f - in) * (1.0f - in);
        const float k = std::min(in, out);
        const float flash = age < 0.6 ? static_cast<float>(1.0 - age / 0.6) : 0.0f;
        const float x = display.x - margin - width + (1.0f - ease) * (width + margin);
        const ImVec2 a{std::floor(x), std::floor(y)}, b{a.x + width, a.y + height};
        const bool accent = notice.kind == NoticeKind::Unlocked || notice.kind == NoticeKind::Completed;
        HudPanel(dl, a, b, s, k, flash, accent);

        const float pad = 14.0f * s, badge = height - pad * 2;
        HudBadge(dl, {a.x + pad, a.y + pad}, badge, TextureFor(notice.badgeUrl), k, s);
        const float tx = a.x + pad * 2 + badge, textWidth = b.x - tx - pad;
        const bool problem = notice.kind == NoticeKind::Problem;
        const ImU32 headerColor = problem ? Col(255, 170, 90, k) : Col(kCyanR, kCyanG, kCyanB, k);
        const char* header = PopupHeader(notice.kind);
        const float headerSize = 14.0f * s;
        dl->AddText(font, headerSize, {tx, a.y + pad - 1.0f * s}, headerColor, header);
        const float headerW = font->CalcTextSizeA(headerSize, FLT_MAX, 0.0f, header).x;
        float pointsW = 0;
        if (notice.points && notice.kind != NoticeKind::GameLoaded) {
            const std::string points = "+" + std::to_string(notice.points);
            const float pointsSize = 20.0f * s;
            pointsW = font->CalcTextSizeA(pointsSize, FLT_MAX, 0.0f, points.c_str()).x;
            dl->AddText(font, pointsSize, {b.x - pad - pointsW, a.y + pad - 5.0f * s}, Col(255, 160, 70, k), points.c_str());
        }
        const float lineY = a.y + pad + headerSize * 0.55f;
        const float lineEnd = b.x - pad - (pointsW > 0 ? pointsW + 8.0f * s : 0.0f);
        if (tx + headerW + 8.0f * s < lineEnd)
            dl->AddLine({tx + headerW + 8.0f * s, lineY}, {lineEnd, lineY}, Col(kCyanR, kCyanG, kCyanB, k * 0.35f), 1.0f * s);
        float ty = a.y + pad + headerSize + 5.0f * s;
        ty += HudText(dl, font, 22.0f * s, {tx, ty}, textWidth, Col(240, 252, 255, k), notice.title, 1) + 2.0f * s;
        HudText(dl, font, 15.0f * s, {tx, ty}, textWidth, Col(170, 220, 240, k * 0.95f), PopupBody(notice), 2);
        y += height + gap;
    }
}

// Progress (a few seconds after it moves) and armed challenges, lower right.
void DrawIndicators(ImFont* font, double now) {
    static uint32_t lastSerial = 0;
    static double shownAt = -100;
    const auto progress = PrimeAchievements::GetProgress();
    if (progress.serial != lastSerial) {
        lastSerial = progress.serial;
        shownAt = now;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float s = std::clamp(display.y / 1080.0f, 0.6f, 2.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float margin = 26.0f * s;
    float bottom = display.y - display.y * 0.2f;
    const double age = now - shownAt;
    if (progress.visible && age < 3.5) {
        const float k = static_cast<float>(std::min(std::clamp(age / 0.25, 0.0, 1.0), std::clamp((3.5 - age) / 0.5, 0.0, 1.0)));
        const float size = 44.0f * s, pad = 8.0f * s, textSize = 18.0f * s;
        const float textW = font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, progress.progress.c_str()).x;
        const float width = size + pad * 3 + textW, height = size + pad * 2;
        const ImVec2 a{display.x - margin - width, bottom - height}, b{display.x - margin, bottom};
        HudPanel(dl, a, b, s * 0.6f, k, 0.0f, false);
        HudBadge(dl, {a.x + pad, a.y + pad}, size, TextureFor(progress.badgeUrl), k, s * 0.7f);
        dl->AddText(font, textSize, {a.x + pad * 2 + size, a.y + (height - textSize) * 0.5f}, Col(240, 252, 255, k),
                    progress.progress.c_str());
        bottom = a.y - 10.0f * s;
    }
    const auto challenges = PrimeAchievements::ChallengeBadges();
    const float size = 40.0f * s, spacing = 8.0f * s;
    float x = display.x - margin - size;
    const float pulse = 0.65f + 0.35f * static_cast<float>(std::sin(now * 4.0));
    for (const auto& url : challenges) {
        const ImVec2 a{x, bottom - size};
        HudBadge(dl, a, size, TextureFor(url), 0.9f, s * 0.7f);
        dl->AddRect({a.x - 2.0f * s, a.y - 2.0f * s}, {a.x + size + 2.0f * s, a.y + size + 2.0f * s},
                    Col(255, 160, 70, pulse), 0.0f, 0, 1.5f * s);
        x -= size + spacing;
    }
}

// --- F2 window ----------------------------------------------------------------
// ImGui keeps its own copies of the text being edited; wipe them after a login.
void WipeImGuiTextCopies() {
    ImGuiContext& g = *GImGui;
    const auto wipe = [](ImVector<char>& text) {
        if (text.Data && text.Capacity > 0) {
#if defined(_WIN32)
            ::SecureZeroMemory(text.Data, static_cast<size_t>(text.Capacity));
#else
            std::memset(text.Data, 0, static_cast<size_t>(text.Capacity));
#endif
        }
    };
    wipe(g.InputTextState.TextA);
    wipe(g.InputTextState.TextToRevertTo);
    wipe(g.InputTextState.CallbackTextBackup);
    g.InputTextState.TextLen = 0;
    wipe(g.InputTextDeactivatedState.TextA);
    ImGui::ClearActiveID();
}

void OpenUrl(const char* url) {
#if defined(_WIN32)
    ::ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)url;
#endif
}

void Image(const std::string& url, float size, bool dim = false) {
    const ImTextureID texture = TextureFor(url);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy({size, size});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + size, pos.y + size}, Col(10, 30, 46, 1.0f));
    if (texture)
        dl->AddImage(texture, pos, {pos.x + size, pos.y + size}, {0, 0}, {1, 1}, dim ? Col(160, 160, 160, 0.8f) : Col(255, 255, 255, 1));
    dl->AddRect(pos, {pos.x + size, pos.y + size}, Col(kCyanR, kCyanG, kCyanB, 0.7f));
}

void DrawAccount() {
    const auto account = PrimeAchievements::GetAccount();
    ImGui::SeparatorText("Account");
    if (account.state == LoginState::LoggedIn) {
        Image(account.avatarUrl, 64);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s", account.displayName.c_str());
        ImGui::TextDisabled("Points (casual): %u    Hardcore: %u",
                            account.softcorePoints, account.hardcorePoints);
        if (ImGui::Button("Log out")) PrimeAchievements::Logout();
        ImGui::SameLine();
        if (ImGui::Button("Open my profile")) {
            const std::string url = "https://retroachievements.org/user/" + account.username;
            OpenUrl(url.c_str());
        }
        ImGui::EndGroup();
        return;
    }
    if (account.state == LoginState::LoggingIn) {
        ImGui::Text("Logging in as %s...", account.username.c_str());
        return;
    }
    static char username[64] = {};
    static char password[128] = {};
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("User", username, sizeof(username), ImGuiInputTextFlags_NoUndoRedo);
    ImGui::SetNextItemWidth(260);
    const bool enter = ImGui::InputText("Password", password, sizeof(password),
                                        ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo |
                                            ImGuiInputTextFlags_EnterReturnsTrue);
    const bool ready = username[0] != 0 && password[0] != 0;
    ImGui::BeginDisabled(!ready);
    const bool clicked = ImGui::Button("Log in");
    ImGui::EndDisabled();
    if ((clicked || enter) && ready) {
        PrimeAchievements::Login(username, password, sizeof(password));  // wipes `password`
        WipeImGuiTextCopies();
    }
    ImGui::SameLine();
    if (ImGui::Button("Create an account")) OpenUrl("https://retroachievements.org/createaccount.php");
    if (account.state == LoginState::Failed && !account.message.empty())
        ImGui::TextColored({1.0f, 0.55f, 0.35f, 1.0f}, "%s", account.message.c_str());
    ImGui::TextDisabled("%s", "Your password is sent once to retroachievements.org over HTTPS and is never saved.\n"
                                 "The session token is kept encrypted for your Windows user (DPAPI) in\n"
                                 "UserData/retroachievements.dat; logging out deletes it.");
}

std::string FormatDate(std::time_t when) {
    if (!when) return {};
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &when);
#else
    localtime_r(&when, &local);
#endif
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

void DrawGame(const PrimeAchievements::Game& game) {
    ImGui::SeparatorText("Game");
    if (!PrimeAchievements::Enabled()) {
        ImGui::TextDisabled("%s", "Turn RetroAchievements on to load this game's achievements.");
        return;
    }
    switch (game.state) {
    case GameState::None:
        ImGui::TextDisabled("%s", "Log in to load the achievements.");
        return;
    case GameState::Loading:
        ImGui::TextDisabled("%s", "Identifying the disc image...");
        return;
    case GameState::Unrecognized:
        ImGui::TextWrapped("This disc image is not one RetroAchievements knows (hash %s). The supported USA image\n"
                              "is the redump dump with hash 19a1c0da8cad36165fb6287c3ea4adde.",
                           game.hash.c_str());
        return;
    case GameState::Failed:
        ImGui::TextColored({1.0f, 0.55f, 0.35f, 1.0f}, "%s", game.message.empty() ? "?" : game.message.c_str());
        return;
    case GameState::Loaded:
        break;
    }
    Image(game.badgeUrl, 64);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%s", game.title.c_str());
    ImGui::Text("%u of %u achievements    %u / %u points (casual)",
                game.unlocked, game.total, game.pointsUnlocked, game.pointsTotal);
    ImGui::SetNextItemWidth(320);
    ImGui::ProgressBar(game.total ? float(game.unlocked) / float(game.total) : 0.0f, {320, 0});
    if (!game.richPresence.empty()) ImGui::TextDisabled("%s", game.richPresence.c_str());
    ImGui::EndGroup();
    if (ImGui::Button("Open the game page")) {
        const std::string url = "https://retroachievements.org/game/" + std::to_string(game.id);
        OpenUrl(url.c_str());
    }
}

void DrawList() {
    static std::vector<PrimeAchievements::AchievementGroup> groups;
    static double refreshed = -10;
    const double now = NowSeconds();
    if (now - refreshed > 1.0) {
        refreshed = now;
        groups = PrimeAchievements::GetAchievements();
    }
    if (groups.empty()) return;
    ImGui::SeparatorText("Achievements");
    if (!ImGui::BeginChild("ra_list", {0, 0}, ImGuiChildFlags_Borders)) {
        ImGui::EndChild();
        return;
    }
    for (const auto& group : groups) {
        ImGui::TextColored({0.38f, 0.84f, 1.0f, 1.0f}, "%s", group.label.c_str());
        for (const auto& entry : group.achievements) {
            ImGui::PushID(static_cast<int>(entry.id));
            Image(entry.unlocked ? entry.badgeUrl : entry.badgeLockedUrl, 48, !entry.unlocked);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Text("%s", entry.title.c_str());
            ImGui::SameLine();
            ImGui::TextColored({1.0f, 0.63f, 0.27f, 1.0f}, "%u", entry.points);
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 8);
            ImGui::TextDisabled("%s", entry.description.c_str());
            ImGui::PopTextWrapPos();
            if (entry.unlocked) {
                const std::string date = FormatDate(entry.unlockTime);
                ImGui::TextColored({0.55f, 1.0f, 0.7f, 1.0f}, "Unlocked %s", date.c_str());
            } else if (!entry.progress.empty()) {
                ImGui::ProgressBar(entry.percent / 100.0f, {220, 0}, entry.progress.c_str());
            }
            ImGui::EndGroup();
            ImGui::PopID();
            ImGui::Spacing();
        }
    }
    ImGui::EndChild();
}
}  // namespace

void DrawPanel(bool* open) {
    PumpImages();
    if (!*open) return;
    ImGui::SetNextWindowSize({640, 760}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    if (ImGui::Begin("RetroAchievements [F2]", open, ImGuiWindowFlags_NoSavedSettings)) {
        if (!PrimeAchievements::Available()) {
            ImGui::TextWrapped("%s", "This build does not include the RetroAchievements client.");
        } else {
            bool enabled = PrimeAchievements::Enabled();
            if (ImGui::Checkbox("Enable RetroAchievements (casual mode)", &enabled))
                PrimeAchievements::SetEnabled(enabled);
            ImGui::TextDisabled("%s", "Connects to retroachievements.org. While on, the custom field of view is off so the\n"
                                         "game plays as shipped. Unlocks count as casual (softcore); hardcore is not offered.");
            bool notifications = PrimeAchievements::NotificationsEnabled();
            if (ImGui::Checkbox("Show unlock pop-ups while playing", &notifications))
                PrimeAchievements::SetNotificationsEnabled(notifications);
            DrawAccount();
            const auto game = PrimeAchievements::GetGame();
            DrawGame(game);
            if (game.state == GameState::Loaded) DrawList();
        }
    }
    ImGui::End();
}

void DrawHud(ImFont* hudFont) {
    PumpImages();
    if (!PrimeAchievements::Enabled()) {
        PrimeAchievements::TakeNotices();
        g_raPopups.clear();
        return;
    }
    ImFont* font = hudFont ? hudFont : ImGui::GetFont();
    const double now = NowSeconds();
    DrawPopups(font, now);
    DrawIndicators(font, now);
}

}  // namespace PrimeAchievementsUi
#endif
