// RetroAchievements (casual mode) for RM3E01: rc_client glue, HTTPS, the
// encrypted session token and badge images. See hle/prime_achievements.h.
#if defined(MKW_GAME_PROFILE_RM3E01_00)
#include "hle/prime_achievements.h"

#include "hle/prime_fov.h"
#include "hle/prime_performance.h"
#include "memory.h"
#include "prime_port_version.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>

#if defined(MKW_PRIME_RCHEEVOS_AVAILABLE)
#include <rc_client.h>
#include <rc_consoles.h>
#include <rc_hash.h>
#if defined(MKW_PRIME_NOD_AVAILABLE)
#include "nod.h"
#endif
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <objidl.h>  // IStream for the GDI+ headers
using std::max;
using std::min;
#include <gdiplus.h>
#endif

namespace PrimeAchievements {
namespace {
std::atomic<bool> g_raLoaded{false}, g_raEnabled{false}, g_raNotifications{true};

void LoadOptions() {
    if (g_raLoaded.exchange(true)) return;
    const auto& config = RuntimeConfigFile::Get();
    g_raEnabled.store(config.primeAchievements.value_or(false));
    g_raNotifications.store(config.primeAchievementsNotifications.value_or(true));
}

// Shared state for the overlay (pop-ups, progress, account, game).
std::mutex g_raMutex;
Account g_raAccount;
Game g_raGame;
std::deque<Notice> g_raNotices;
Progress g_raProgress;
std::vector<std::pair<uint32_t, std::string>> g_raChallenges;  // achievement id, badge

void PushNotice(Notice notice) {
    std::lock_guard lock(g_raMutex);
    if (g_raNotices.size() >= 16) g_raNotices.pop_front();
    g_raNotices.push_back(std::move(notice));
}

#if defined(_WIN32)
std::wstring WidenUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(length, 0)), L'\0');
    if (length > 0) ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// One HTTPS request (never plain HTTP). The body of a login carries the
// password or token: it is neither logged nor kept after the call.
struct HttpResult {
    int status = 0;  // 0: no response (network error)
    std::string body;
};
HttpResult HttpsRequest(const std::string& url, const std::string* post, const char* contentType, const std::wstring& agent) {
    HttpResult result;
    const std::wstring wideUrl = WidenUtf8(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!::WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return result;
    HINTERNET session = ::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;
    ::WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    HINTERNET connection = ::WinHttpConnect(session, host, parts.nPort, 0);
    if (connection) {
        const std::wstring target = std::wstring(path) + extra;
        HINTERNET request = ::WinHttpOpenRequest(connection, post ? L"POST" : L"GET", target.c_str(), nullptr,
                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request) {
            std::wstring headers;
            if (post) headers = L"Content-Type: " + WidenUtf8(contentType ? contentType : "application/x-www-form-urlencoded") + L"\r\n";
            const BOOL sent = ::WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                                   headers.empty() ? 0 : static_cast<DWORD>(-1),
                                                   post ? const_cast<char*>(post->data()) : WINHTTP_NO_REQUEST_DATA,
                                                   post ? static_cast<DWORD>(post->size()) : 0,
                                                   post ? static_cast<DWORD>(post->size()) : 0, 0);
            if (sent && ::WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                      &status, &size, WINHTTP_NO_HEADER_INDEX);
                result.status = static_cast<int>(status);
                DWORD available = 0;
                while (::WinHttpQueryDataAvailable(request, &available) && available > 0 && result.body.size() < (16u << 20)) {
                    const size_t offset = result.body.size();
                    result.body.resize(offset + available);
                    DWORD read = 0;
                    if (!::WinHttpReadData(request, result.body.data() + offset, available, &read)) { result.body.resize(offset); break; }
                    result.body.resize(offset + read);
                    if (read == 0) break;
                }
            }
            ::WinHttpCloseHandle(request);
        }
        ::WinHttpCloseHandle(connection);
    }
    ::WinHttpCloseHandle(session);
    return result;
}

// --- session token, DPAPI ---------------------------------------------------
constexpr char kTokenEntropy[] = "Corruption_3 PC/RetroAchievements/token";
std::filesystem::path CredentialsPath() { return RuntimeConfigFile::ApplicationDataDirectory() / "retroachievements.dat"; }

bool SaveCredentials(const std::string& username, const std::string& token) {
    std::string plain = username + '\n' + token;
    DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(plain.data())};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kTokenEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kTokenEntropy))};
    DATA_BLOB output{};
    const bool ok = ::CryptProtectData(&input, L"Corruption_3 PC RetroAchievements session", &entropy, nullptr, nullptr,
                                       CRYPTPROTECT_UI_FORBIDDEN, &output) != FALSE;
    ::SecureZeroMemory(plain.data(), plain.size());
    if (!ok) return false;
    std::error_code error;
    std::filesystem::create_directories(CredentialsPath().parent_path(), error);
    const auto temporary = CredentialsPath().string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
    }
    ::LocalFree(output.pbData);
    std::filesystem::rename(temporary, CredentialsPath(), error);
    return !error;
}

bool LoadCredentials(std::string& username, std::string& token) {
    std::ifstream file(CredentialsPath(), std::ios::binary);
    if (!file) return false;
    std::string blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    DATA_BLOB input{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(blob.data())};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kTokenEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kTokenEntropy))};
    DATA_BLOB output{};
    if (!::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    std::string plain(reinterpret_cast<const char*>(output.pbData), output.cbData);
    ::SecureZeroMemory(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    const size_t split = plain.find('\n');
    const bool ok = split != std::string::npos && split > 0 && split + 1 < plain.size();
    if (ok) {
        username = plain.substr(0, split);
        token = plain.substr(split + 1);
    }
    ::SecureZeroMemory(plain.data(), plain.size());
    return ok;
}

void DeleteCredentials() {
    std::error_code error;
    std::filesystem::remove(CredentialsPath(), error);
}

// --- images ---------------------------------------------------------------
std::mutex g_raImageMutex;
std::condition_variable g_raImageWake;
std::deque<std::string> g_raImageQueue;
std::vector<std::string> g_raImagesRequested;
std::vector<Image> g_raImagesReady;
std::once_flag g_raImageThreadStarted;

bool DecodePng(const std::string& bytes, Image& image) {
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!memory) return false;
    std::memcpy(::GlobalLock(memory), bytes.data(), bytes.size());
    ::GlobalUnlock(memory);
    IStream* stream = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(memory, TRUE, &stream))) { ::GlobalFree(memory); return false; }
    bool ok = false;
    {
        Gdiplus::Bitmap bitmap(stream);
        if (bitmap.GetLastStatus() == Gdiplus::Ok && bitmap.GetWidth() > 0 && bitmap.GetWidth() <= 1024 &&
            bitmap.GetHeight() > 0 && bitmap.GetHeight() <= 1024) {
            image.width = bitmap.GetWidth();
            image.height = bitmap.GetHeight();
            Gdiplus::Rect rect(0, 0, static_cast<INT>(image.width), static_cast<INT>(image.height));
            Gdiplus::BitmapData data{};
            if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
                image.rgba.resize(size_t(image.width) * image.height * 4);
                for (uint32_t y = 0; y < image.height; ++y) {
                    const auto* row = static_cast<const uint8_t*>(data.Scan0) + ptrdiff_t(y) * data.Stride;
                    uint8_t* out = image.rgba.data() + size_t(y) * image.width * 4;
                    for (uint32_t x = 0; x < image.width; ++x) {  // BGRA -> RGBA
                        out[x * 4 + 0] = row[x * 4 + 2];
                        out[x * 4 + 1] = row[x * 4 + 1];
                        out[x * 4 + 2] = row[x * 4 + 0];
                        out[x * 4 + 3] = row[x * 4 + 3];
                    }
                }
                bitmap.UnlockBits(&data);
                ok = true;
            }
        }
    }
    stream->Release();
    return ok;
}

void ImageWorker() {
    ::SetThreadDescription(::GetCurrentThread(), L"RetroAchievements images");
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR gdiplus = 0;
    const bool gdiplusOk = Gdiplus::GdiplusStartup(&gdiplus, &input, nullptr) == Gdiplus::Ok;
    const std::wstring agent = WidenUtf8(std::string("Corruption3PC/") + PRIME_PORT_VERSION);
    for (;;) {
        std::string url;
        {
            std::unique_lock lock(g_raImageMutex);
            g_raImageWake.wait(lock, [] { return !g_raImageQueue.empty(); });
            url = std::move(g_raImageQueue.front());
            g_raImageQueue.pop_front();
        }
        Image image;
        image.url = url;
        const HttpResult response = HttpsRequest(url, nullptr, nullptr, agent);
        if (!(gdiplusOk && response.status == 200 && DecodePng(response.body, image))) image.width = image.height = 0;
        std::lock_guard lock(g_raImageMutex);
        g_raImagesReady.push_back(std::move(image));
    }
}
#endif  // _WIN32

#if defined(MKW_PRIME_RCHEEVOS_AVAILABLE) && defined(_WIN32)
rc_client_t* g_raClient = nullptr;
std::once_flag g_raClientCreated;
std::atomic<bool> g_raLoggedIn{false}, g_raGameRequested{false};

// --- server calls: one worker, requests in order -------------------------
struct ServerJob {
    std::string url, post, contentType;
    bool hasPost = false;
    rc_client_server_callback_t callback = nullptr;
    void* callbackData = nullptr;
};
std::mutex g_raJobMutex;
std::condition_variable g_raJobWake;
std::deque<ServerJob> g_raJobs;

std::wstring UserAgent() {
    char clause[128] = {};
    if (g_raClient) rc_client_get_user_agent_clause(g_raClient, clause, sizeof(clause));
    return WidenUtf8(std::string("Corruption3PC/") + PRIME_PORT_VERSION + " (Windows) " + clause);
}

void ServerWorker() {
    ::SetThreadDescription(::GetCurrentThread(), L"RetroAchievements server");
    for (;;) {
        ServerJob job;
        {
            std::unique_lock lock(g_raJobMutex);
            g_raJobWake.wait(lock, [] { return !g_raJobs.empty(); });
            job = std::move(g_raJobs.front());
            g_raJobs.pop_front();
        }
        const HttpResult http = HttpsRequest(job.url, job.hasPost ? &job.post : nullptr, job.contentType.c_str(), UserAgent());
        if (!job.post.empty()) ::SecureZeroMemory(job.post.data(), job.post.size());
        rc_api_server_response_t response{};
        response.body = http.body.c_str();
        response.body_length = http.body.size();
        response.http_status_code = http.status ? http.status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        job.callback(&response, job.callbackData);
    }
}

void RC_CCONV ServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* data, rc_client_t*) {
    ServerJob job;
    job.url = request->url ? request->url : "";
    job.hasPost = request->post_data != nullptr;
    if (request->post_data) job.post = request->post_data;
    job.contentType = request->content_type ? request->content_type : "application/x-www-form-urlencoded";
    job.callback = callback;
    job.callbackData = data;
    {
        std::lock_guard lock(g_raJobMutex);
        g_raJobs.push_back(std::move(job));
    }
    g_raJobWake.notify_one();
}

// rcheevos' Wii map: 0x00000000-0x017FFFFF MEM1 (0x80000000), 0x10000000-0x13FFFFFF
// MEM2 (0x90000000). Guest memory holds the console's big-endian bytes.
uint32_t RC_CCONV ReadMemory(uint32_t address, uint8_t* buffer, uint32_t count, rc_client_t*) {
    uint32_t guest = 0;
    if (uint64_t(address) + count <= 0x01800000u) guest = 0x80000000u + address;
    else if (address >= 0x10000000u && uint64_t(address) + count <= 0x14000000u) guest = 0x90000000u + (address - 0x10000000u);
    else return 0;
    if (count == 0 || !Memory::Contains(guest, count)) return 0;
    std::memcpy(buffer, Memory::GetPointer(guest, count), count);
    return count;
}

// Warnings only, and never one that could quote a request: logs go into bug reports.
void RC_CCONV LogMessage(const char* message, const rc_client_t*) {
    const std::string_view text = message ? message : "";
    for (const std::string_view secret : {"t=", "p=", "token", "password", "Token", "Password"})
        if (text.find(secret) != std::string_view::npos) {
            RT_LOG(RT_TAG_RUNTIME) << "[retroachievements] (message withheld: it may quote credentials)" << std::endl;
            return;
        }
    RT_LOG(RT_TAG_RUNTIME) << "[retroachievements] " << text << std::endl;
}

std::string BadgeOf(const rc_client_achievement_t* achievement, bool unlocked) {
    if (!achievement) return {};
    const char* url = unlocked ? achievement->badge_url : achievement->badge_locked_url;
    return url ? url : "";
}

void RefreshGameSummary() {
    const rc_client_game_t* game = rc_client_get_game_info(g_raClient);
    rc_client_user_game_summary_t summary{};
    rc_client_get_user_game_summary(g_raClient, &summary);
    std::lock_guard lock(g_raMutex);
    if (!game) return;
    g_raGame.id = game->id;
    g_raGame.title = game->title ? game->title : "";
    g_raGame.badgeUrl = game->badge_url ? game->badge_url : "";
    g_raGame.hash = game->hash ? game->hash : "";
    g_raGame.unlocked = summary.num_unlocked_achievements;
    g_raGame.total = summary.num_core_achievements;
    g_raGame.pointsUnlocked = summary.points_unlocked;
    g_raGame.pointsTotal = summary.points_core;
}

void RefreshAccount() {
    const rc_client_user_t* user = rc_client_get_user_info(g_raClient);
    std::lock_guard lock(g_raMutex);
    if (!user) return;
    g_raAccount.username = user->username ? user->username : "";
    g_raAccount.displayName = user->display_name ? user->display_name : g_raAccount.username;
    g_raAccount.avatarUrl = user->avatar_url ? user->avatar_url : "";
    g_raAccount.softcorePoints = user->score_softcore;
    g_raAccount.hardcorePoints = user->score;
}

void RC_CCONV EventHandler(const rc_client_event_t* event, rc_client_t*) {
    const bool notify = g_raNotifications.load();
    switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        if (event->achievement) {
            RT_LOG(RT_TAG_RUNTIME) << "[retroachievements] unlocked " << event->achievement->id << " "
                                   << (event->achievement->title ? event->achievement->title : "") << std::endl;
            if (notify)
                PushNotice({NoticeKind::Unlocked, event->achievement->title ? event->achievement->title : "",
                            event->achievement->description ? event->achievement->description : "",
                            BadgeOf(event->achievement, true), event->achievement->points});
        }
        RefreshGameSummary();
        RefreshAccount();
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED: {
        RefreshGameSummary();
        Game game;
        {
            std::lock_guard lock(g_raMutex);
            game = g_raGame;
        }
        if (notify) PushNotice({NoticeKind::Completed, game.title, "", game.badgeUrl, game.pointsTotal});
        break;
    }
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
        if (event->achievement) {
            std::lock_guard lock(g_raMutex);
            g_raProgress.visible = true;
            g_raProgress.title = event->achievement->title ? event->achievement->title : "";
            g_raProgress.progress = event->achievement->measured_progress;
            g_raProgress.badgeUrl = BadgeOf(event->achievement, false);
            ++g_raProgress.serial;
        }
        break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE: {
        std::lock_guard lock(g_raMutex);
        g_raProgress.visible = false;
        break;
    }
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
        if (event->achievement) {
            std::lock_guard lock(g_raMutex);
            g_raChallenges.emplace_back(event->achievement->id, BadgeOf(event->achievement, true));
        }
        break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
        if (event->achievement) {
            std::lock_guard lock(g_raMutex);
            const uint32_t id = event->achievement->id;
            g_raChallenges.erase(std::remove_if(g_raChallenges.begin(), g_raChallenges.end(),
                                                [id](const auto& entry) { return entry.first == id; }),
                                 g_raChallenges.end());
        }
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        if (event->server_error) {
            RT_LOG(RT_TAG_RUNTIME) << "[retroachievements] server error: "
                                   << (event->server_error->error_message ? event->server_error->error_message : "?") << std::endl;
            PushNotice({NoticeKind::Problem, "RetroAchievements",
                        event->server_error->error_message ? event->server_error->error_message : "", "", 0});
        }
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        PushNotice({NoticeKind::Problem, "RetroAchievements", "offline: unlocks will be sent when the connection returns", "", 0});
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        PushNotice({NoticeKind::Info, "RetroAchievements", "connection restored, pending unlocks sent", "", 0});
        break;
    default:
        break;
    }
}

void RC_CCONV GameLoaded(int result, const char* message, rc_client_t*, void*) {
    const rc_client_game_t* game = rc_client_get_game_info(g_raClient);
    if (result == RC_OK && game && game->id != 0) {
        {
            std::lock_guard lock(g_raMutex);
            g_raGame.state = GameState::Loaded;
            g_raGame.message.clear();
        }
        RefreshGameSummary();
        Game snapshot;
        {
            std::lock_guard lock(g_raMutex);
            snapshot = g_raGame;
        }
        RT_LOG(RT_TAG_RUNTIME) << "[retroachievements] game " << snapshot.id << " loaded: " << snapshot.unlocked << "/"
                               << snapshot.total << " unlocked (casual)" << std::endl;
        if (g_raNotifications.load())
            PushNotice({NoticeKind::GameLoaded, snapshot.title,
                        std::to_string(snapshot.unlocked) + "/" + std::to_string(snapshot.total), snapshot.badgeUrl,
                        snapshot.pointsUnlocked});
        return;
    }
    std::lock_guard lock(g_raMutex);
    g_raGame.state = (result == RC_OK || result == RC_NO_GAME_LOADED) ? GameState::Unrecognized : GameState::Failed;
    g_raGame.hash = game && game->hash ? game->hash : g_raGame.hash;
    g_raGame.message = message ? message : "";
    RT_LOG(RT_TAG_RUNTIME) << "[retroachievements] game not loaded (" << result << "): " << g_raGame.message << std::endl;
}

// Hashing reads ~32 MB of the image: off the game and UI threads.
void BeginLoadGame() {
    if (g_raGameRequested.exchange(true)) return;
    {
        std::lock_guard lock(g_raMutex);
        g_raGame = Game{};
        g_raGame.state = GameState::Loading;
    }
    std::thread([] {
        ::SetThreadDescription(::GetCurrentThread(), L"RetroAchievements identify");
        const auto path = RuntimeConfigFile::ResolvedPrimeIsoPath();
        const std::string utf8 = RuntimeConfigFile::PathToUtf8(path);
        rc_client_begin_identify_and_load_game(g_raClient, RC_CONSOLE_WII, utf8.c_str(), nullptr, 0, GameLoaded, nullptr);
    }).detach();
}

void RC_CCONV LoggedIn(int result, const char* message, rc_client_t*, void* fromPassword) {
    if (result == RC_OK) {
        const rc_client_user_t* user = rc_client_get_user_info(g_raClient);
        if (user && user->username && user->token && fromPassword) SaveCredentials(user->username, user->token);
        RefreshAccount();
        {
            std::lock_guard lock(g_raMutex);
            g_raAccount.state = LoginState::LoggedIn;
            g_raAccount.message.clear();
        }
        g_raLoggedIn.store(true);
        RT_LOG(RT_TAG_RUNTIME) << "[retroachievements] logged in" << std::endl;
        if (g_raEnabled.load()) BeginLoadGame();
        return;
    }
    // A rejected saved token is useless: forget it and ask for the password.
    if (!fromPassword && (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN)) {
        DeleteCredentials();
        PushNotice({NoticeKind::Problem, "RetroAchievements", "session expired: log in again in F2", "", 0});
    }
    std::lock_guard lock(g_raMutex);
    g_raAccount.state = LoginState::Failed;
    g_raAccount.message = message ? message : "login failed";
    RT_LOG(RT_TAG_RUNTIME) << "[retroachievements] login failed (" << result << ")" << std::endl;
}

#if defined(MKW_PRIME_NOD_AVAILABLE)
// The disc hash covers the original disc bytes, but rcheevos reads the file as
// is. nod rebuilds those bytes from any container, so an .rvz gets the same hash
// as the plain .iso (19a1c0da8cad36165fb6287c3ea4adde was checked on both).
void* RC_CCONV DiscOpen(const char* path) {
    NodHandle* disc = nullptr;
    return nod_disc_open(path, nullptr, &disc) == NOD_RESULT_OK ? disc : nullptr;
}
void RC_CCONV DiscSeek(void* disc, int64_t offset, int origin) {
    nod_seek(static_cast<NodHandle*>(disc), offset, origin);
}
int64_t RC_CCONV DiscTell(void* disc) {
    return nod_seek(static_cast<NodHandle*>(disc), 0, SEEK_CUR);
}
size_t RC_CCONV DiscRead(void* disc, void* buffer, size_t bytes) {
    size_t done = 0;
    while (done < bytes) {
        const int64_t count = nod_read(static_cast<NodHandle*>(disc), static_cast<uint8_t*>(buffer) + done, bytes - done);
        if (count <= 0) break;
        done += static_cast<size_t>(count);
    }
    return done;
}
void RC_CCONV DiscClose(void* disc) {
    nod_free(static_cast<NodHandle*>(disc));
}
#endif

void EnsureClient() {
    std::call_once(g_raClientCreated, [] {
        g_raClient = rc_client_create(ReadMemory, ServerCall);
#if defined(MKW_PRIME_NOD_AVAILABLE)
        rc_hash_callbacks_t hash{};
        hash.filereader = {DiscOpen, DiscSeek, DiscTell, DiscRead, DiscClose};
        rc_client_set_hash_callbacks(g_raClient, &hash);
#endif
        rc_client_enable_logging(g_raClient, RC_CLIENT_LOG_LEVEL_WARN, LogMessage);
        rc_client_set_event_handler(g_raClient, EventHandler);
        rc_client_set_hardcore_enabled(g_raClient, 0);  // casual only (decision of 2026-10-31)
        std::thread(ServerWorker).detach();
    });
}

void LoginWithSavedToken() {
    std::string username, token;
    if (!LoadCredentials(username, token)) return;
    {
        std::lock_guard lock(g_raMutex);
        g_raAccount.state = LoginState::LoggingIn;
        g_raAccount.username = username;
    }
    rc_client_begin_login_with_token(g_raClient, username.c_str(), token.c_str(), LoggedIn, nullptr);
    ::SecureZeroMemory(token.data(), token.size());
}

void Start() {
    EnsureClient();
    if (g_raLoggedIn.load()) {
        BeginLoadGame();
        return;
    }
    LoginState state;
    {
        std::lock_guard lock(g_raMutex);
        state = g_raAccount.state;
    }
    if (state != LoginState::LoggingIn) LoginWithSavedToken();
}
#endif  // MKW_PRIME_RCHEEVOS_AVAILABLE && _WIN32
}  // namespace

bool Available() {
#if defined(MKW_PRIME_RCHEEVOS_AVAILABLE) && defined(_WIN32)
    return true;
#else
    return false;
#endif
}

bool Enabled() { LoadOptions(); return g_raEnabled.load() && Available(); }
bool NotificationsEnabled() { LoadOptions(); return g_raNotifications.load(); }
void SetNotificationsEnabled(bool enabled) {
    LoadOptions();
    g_raNotifications.store(enabled);
    RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeAchievementsNotifications, "achievements_notifications", enabled);
}

void SetEnabled(bool enabled) {
    LoadOptions();
    g_raEnabled.store(enabled);
    RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeAchievements, "achievements", enabled);
    // The game as shipped while achievements run.
    PrimeFov::SetBlocked(enabled);
#if defined(MKW_PRIME_RCHEEVOS_AVAILABLE) && defined(_WIN32)
    if (enabled) {
        Start();
    } else if (g_raClient) {
        rc_client_unload_game(g_raClient);
        g_raGameRequested.store(false);
        std::lock_guard lock(g_raMutex);
        g_raGame = Game{};
        g_raProgress = Progress{};
        g_raChallenges.clear();
    }
#endif
}

Account GetAccount() {
    std::lock_guard lock(g_raMutex);
    return g_raAccount;
}

void Login(const std::string& username, char* password, size_t capacity) {
#if defined(MKW_PRIME_RCHEEVOS_AVAILABLE) && defined(_WIN32)
    EnsureClient();
    {
        std::lock_guard lock(g_raMutex);
        g_raAccount.state = LoginState::LoggingIn;
        g_raAccount.username = username;
        g_raAccount.message.clear();
    }
    // rc_client builds the request (it copies the password into the POST body,
    // which the server worker wipes after sending) before this returns.
    rc_client_begin_login_with_password(g_raClient, username.c_str(), password, LoggedIn, reinterpret_cast<void*>(1));
#endif
    if (password && capacity) ::SecureZeroMemory(password, capacity);
}

void Logout() {
#if defined(MKW_PRIME_RCHEEVOS_AVAILABLE) && defined(_WIN32)
    if (g_raClient) {
        rc_client_unload_game(g_raClient);
        rc_client_logout(g_raClient);
    }
    DeleteCredentials();
    g_raLoggedIn.store(false);
    g_raGameRequested.store(false);
    std::lock_guard lock(g_raMutex);
    g_raAccount = Account{};
    g_raGame = Game{};
    g_raProgress = Progress{};
    g_raChallenges.clear();
    RT_LOG(RT_TAG_RUNTIME) << "[retroachievements] logged out, saved session deleted" << std::endl;
#endif
}

Game GetGame() {
    Game game;
    {
        std::lock_guard lock(g_raMutex);
        game = g_raGame;
    }
#if defined(MKW_PRIME_RCHEEVOS_AVAILABLE) && defined(_WIN32)
    if (g_raClient && game.state == GameState::Loaded) {
        char text[256] = {};
        rc_client_get_rich_presence_message(g_raClient, text, sizeof(text));
        game.richPresence = text;
    }
#endif
    return game;
}

std::vector<AchievementGroup> GetAchievements() {
    std::vector<AchievementGroup> groups;
#if defined(MKW_PRIME_RCHEEVOS_AVAILABLE) && defined(_WIN32)
    if (!g_raClient || !rc_client_is_game_loaded(g_raClient)) return groups;
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(
        g_raClient, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
    if (!list) return groups;
    for (uint32_t b = 0; b < list->num_buckets; ++b) {
        const auto& bucket = list->buckets[b];
        AchievementGroup group;
        group.label = bucket.label ? bucket.label : "";
        for (uint32_t i = 0; i < bucket.num_achievements; ++i) {
            const rc_client_achievement_t* source = bucket.achievements[i];
            Achievement entry;
            entry.id = source->id;
            entry.points = source->points;
            entry.title = source->title ? source->title : "";
            entry.description = source->description ? source->description : "";
            entry.badgeUrl = BadgeOf(source, true);
            entry.badgeLockedUrl = BadgeOf(source, false);
            entry.progress = source->measured_progress;
            entry.percent = source->measured_percent;
            entry.rarity = source->rarity;
            entry.unlocked = source->unlocked != 0;
            entry.unlockTime = source->unlock_time;
            group.achievements.push_back(std::move(entry));
        }
        groups.push_back(std::move(group));
    }
    rc_client_destroy_achievement_list(list);
#endif
    return groups;
}

std::vector<Notice> TakeNotices() {
    std::lock_guard lock(g_raMutex);
    std::vector<Notice> notices(g_raNotices.begin(), g_raNotices.end());
    g_raNotices.clear();
    return notices;
}

Progress GetProgress() {
    std::lock_guard lock(g_raMutex);
    return g_raProgress;
}

std::vector<std::string> ChallengeBadges() {
    std::lock_guard lock(g_raMutex);
    std::vector<std::string> badges;
    for (const auto& entry : g_raChallenges) badges.push_back(entry.second);
    return badges;
}

void RequestImage(const std::string& url) {
#if defined(_WIN32)
    if (url.rfind("https://", 0) != 0) return;
    std::call_once(g_raImageThreadStarted, [] { std::thread(ImageWorker).detach(); });
    {
        std::lock_guard lock(g_raImageMutex);
        if (std::find(g_raImagesRequested.begin(), g_raImagesRequested.end(), url) != g_raImagesRequested.end()) return;
        g_raImagesRequested.push_back(url);
        g_raImageQueue.push_back(url);
    }
    g_raImageWake.notify_one();
#else
    (void)url;
#endif
}

std::vector<Image> TakeImages() {
#if defined(_WIN32)
    std::lock_guard lock(g_raImageMutex);
    std::vector<Image> images = std::move(g_raImagesReady);
    g_raImagesReady.clear();
    return images;
#else
    return {};
#endif
}

void Frame() {
    LoadOptions();
#if defined(MKW_PRIME_RCHEEVOS_AVAILABLE) && defined(_WIN32)
    static bool started = false;
    const bool enabled = g_raEnabled.load();
    if (!started) {
        started = true;
        PrimeFov::SetBlocked(enabled);
        if (enabled) Start();
    }
    if (!enabled || !g_raClient) return;
    if (!PrimePerformance::Enabled()) {
        rc_client_do_frame(g_raClient);
        return;
    }
    // Performance trace sessions: the cost on the game thread, every 10 s.
    static uint64_t frames = 0, totalNs = 0, maxNs = 0;
    const auto begin = std::chrono::steady_clock::now();
    rc_client_do_frame(g_raClient);
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
    totalNs += ns;
    maxNs = std::max(maxNs, ns);
    if (++frames == 600) {
        RT_LOG(RT_TAG_RUNTIME) << "[retroachievements-perf] frames=600 avg_us=" << double(totalNs) / 600.0 / 1000.0
                               << " max_us=" << double(maxNs) / 1000.0 << std::endl;
        frames = totalNs = maxNs = 0;
    }
#endif
}

}  // namespace PrimeAchievements
#endif
