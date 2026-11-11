#pragma once
// RetroAchievements for RM3E01, casual (softcore) mode only, through the
// official rcheevos rc_client (MIT, recomp/.tools/rcheevos, v12.5.0). Design:
// recomp/docs/retroachievements.md.
//
// The game's achievement definitions are conditions on console memory. RM3E01
// runs translated code over the Wii's own memory image (MEM1 at 0x80000000,
// MEM2 at 0x90000000, big-endian), which is exactly rcheevos' Wii map, so the
// definitions written for Dolphin read the same bytes here. The disc is
// identified by rcheevos' hash of the configured ISO (read-only).
//
// Credentials: the password is sent once over HTTPS and never stored; the
// session token rc_client returns is kept for this Windows user only, encrypted
// with DPAPI in UserData/retroachievements.dat (not in Config.toml, which bug
// reports include). Nothing secret is logged.
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace PrimeAchievements {

// Whether rcheevos is part of this build.
bool Available();

// [prime] achievements (off by default). Turning it on logs in with the saved
// token, if any; while on, the custom field of view stays off (the game as
// shipped). Turning it off unloads the game from rc_client and keeps the token.
bool Enabled();
void SetEnabled(bool enabled);
// [prime] achievements_notifications (on): unlock pop-ups while playing.
bool NotificationsEnabled();
void SetNotificationsEnabled(bool enabled);

enum class LoginState : uint8_t { LoggedOut, LoggingIn, LoggedIn, Failed };
struct Account {
    LoginState state = LoginState::LoggedOut;
    std::string username, displayName, avatarUrl, message;
    uint32_t softcorePoints = 0, hardcorePoints = 0;
};
Account GetAccount();
// Starts a login; `password` is wiped (SecureZeroMemory) before returning.
void Login(const std::string& username, char* password, size_t passwordCapacity);
// Ends the session and deletes the saved token.
void Logout();

enum class GameState : uint8_t { None, Loading, Loaded, Unrecognized, Failed };
struct Game {
    GameState state = GameState::None;
    uint32_t id = 0;
    std::string title, badgeUrl, hash, message, richPresence;
    uint32_t unlocked = 0, total = 0, pointsUnlocked = 0, pointsTotal = 0;
};
Game GetGame();

struct Achievement {
    uint32_t id = 0, points = 0;
    std::string title, description, badgeUrl, badgeLockedUrl, progress;
    float percent = 0, rarity = 0;
    bool unlocked = false;
    std::time_t unlockTime = 0;
};
struct AchievementGroup {
    std::string label;
    std::vector<Achievement> achievements;
};
std::vector<AchievementGroup> GetAchievements();

// Pop-ups, oldest first; the overlay takes them once.
enum class NoticeKind : uint8_t { Unlocked, GameLoaded, Completed, Info, Problem };
struct Notice {
    NoticeKind kind = NoticeKind::Info;
    std::string title, text, badgeUrl;
    uint32_t points = 0;
};
std::vector<Notice> TakeNotices();

// Progress of an achievement the player is working on (shown briefly after it
// moves) and the challenges currently armed.
struct Progress {
    bool visible = false;
    std::string title, progress, badgeUrl;
    uint32_t serial = 0;  // bumps on every update
};
Progress GetProgress();
std::vector<std::string> ChallengeBadges();

// Badges and avatars: PNGs fetched over HTTPS and decoded off the UI thread.
struct Image {
    std::string url;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
void RequestImage(const std::string& url);
std::vector<Image> TakeImages();

// Game thread, once per retrace.
void Frame();

}  // namespace PrimeAchievements
