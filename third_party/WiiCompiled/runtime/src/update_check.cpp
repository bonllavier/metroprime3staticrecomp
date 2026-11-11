#include "update_check.h"

#include "prime_port_version.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#else
#include "host_https.h"
#include <SDL3/SDL.h>
#endif

namespace UpdateCheck {
namespace {

std::mutex g_mutex;
Status g_status;
std::atomic_bool g_running{false};

void Set(const Status& status) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = status;
}

// "https://github.com/<owner>/<repo>" -> owner, repo
bool RepositoryParts(std::string& owner, std::string& repo) {
    const std::string url = PRIME_PORT_REPOSITORY;
    const auto host = url.find("github.com/");
    if (host == std::string::npos) return false;
    const std::string path = url.substr(host + 11);
    const auto slash = path.find('/');
    if (slash == std::string::npos) return false;
    owner = path.substr(0, slash);
    repo = path.substr(slash + 1);
    while (!repo.empty() && (repo.back() == '/' || repo.back() == '\n')) repo.pop_back();
    return !owner.empty() && !repo.empty();
}

std::array<int, 3> ParseVersion(std::string_view text) {
    std::array<int, 3> v{0, 0, 0};
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    size_t part = 0;
    int current = 0;
    bool any = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') { current = current * 10 + (c - '0'); any = true; }
        else if (c == '.') { if (part < 3) v[part] = current; ++part; current = 0; any = false; }
        else break;
    }
    if (any && part < 3) v[part] = current;
    return v;
}

std::string JsonString(const std::string& body, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return {};
    pos = body.find('"', body.find(':', pos));
    if (pos == std::string::npos) return {};
    const auto end = body.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return body.substr(pos + 1, end - pos - 1);
}

#if defined(_WIN32)
std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

bool HttpsGet(const std::wstring& host, const std::wstring& path, std::string& body, std::string& error) {
    const std::wstring agent = Widen(std::string("Corruption3PC/") + PRIME_PORT_VERSION);
    HINTERNET session = ::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = "WinHttpOpen failed"; return false; }
    ::WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
    HINTERNET connection = ::WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) { ::WinHttpCloseHandle(session); error = "no connection to " + std::string(host.begin(), host.end()); return false; }
    HINTERNET request = ::WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    bool ok = false;
    if (request) {
        const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (::WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && ::WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, statusSize = sizeof(status);
            ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
            DWORD available = 0;
            while (::WinHttpQueryDataAvailable(request, &available) && available > 0) {
                std::vector<char> chunk(available);
                DWORD read = 0;
                if (!::WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
                body.append(chunk.data(), read);
                if (body.size() > 1u << 20) break;
            }
            if (status == 200) ok = true;
            else error = "HTTP " + std::to_string(status) + (status == 404 ? " (repository not found)" : "");
        } else {
            error = "request failed (error " + std::to_string(::GetLastError()) + ", offline?)";
        }
        ::WinHttpCloseHandle(request);
    } else {
        error = "WinHttpOpenRequest failed";
    }
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    return ok;
}
#else
bool HttpsGet(const std::string& host, const std::string& path, std::string& body, std::string& error) {
    const HostHttps::Response response = HostHttps::Request(
        "https://" + host + path, nullptr,
        {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"},
        std::string("Corruption3PC/") + PRIME_PORT_VERSION, 8);
    if (!response.ok) {
        error = "request failed (" + response.error + ", offline?)";
        return false;
    }
    body = response.body.size() > (1u << 20) ? response.body.substr(0, 1u << 20) : response.body;
    if (response.status == 200) return true;
    error = "HTTP " + std::to_string(response.status) + (response.status == 404 ? " (repository not found)" : "");
    return false;
}
#endif

void Worker() {
    Status status;
    status.state = State::Checking;
    Set(status);
    std::string owner, repo;
    if (!RepositoryParts(owner, repo)) {
        status.state = State::Failed; status.message = "repository URL not configured";
        Set(status); g_running.store(false); return;
    }
    std::string body, error;
    // Anonymous, public endpoint (no token). /releases/latest skips prereleases,
    // and every 0.x build is published as one, so read the list and take the
    // highest version (drafts are never listed without authentication).
#if defined(_WIN32)
    const std::wstring path = Widen("/repos/" + owner + "/" + repo + "/releases?per_page=30");
    if (!HttpsGet(L"api.github.com", path, body, error)) {
#else
    if (!HttpsGet("api.github.com", "/repos/" + owner + "/" + repo + "/releases?per_page=30", body, error)) {
#endif
        status.state = State::Failed; status.message = error;
        RT_LOG(RT_TAG_RUNTIME) << "[update-check] failed: " << error << std::endl;
        Set(status); g_running.store(false); return;
    }
    std::string tag;
    std::array<int, 3> best{-1, -1, -1};
    for (size_t pos = body.find("\"tag_name\""); pos != std::string::npos; pos = body.find("\"tag_name\"", pos + 1)) {
        const std::string candidate = JsonString(body.substr(pos), "tag_name");
        if (candidate.empty()) continue;
        const auto version = ParseVersion(candidate);
        if (version > best) { best = version; tag = candidate; }
    }
    status.downloadUrl = std::string(PRIME_PORT_REPOSITORY) + (tag.empty() ? "/releases" : "/releases/tag/" + tag);
    if (tag.empty()) {
        status.state = State::Failed;
        status.message = body.find('[') != std::string::npos ? "no release published yet" : "unexpected reply from GitHub";
    } else {
        const auto latest = ParseVersion(tag), current = ParseVersion(PRIME_PORT_VERSION);
        status.latestVersion = tag.front() == 'v' ? tag.substr(1) : tag;
        status.state = latest > current ? State::UpdateAvailable : State::UpToDate;
        status.message = latest > current ? "A newer build is available" : "This is the latest build";
    }
    RT_LOG(RT_TAG_RUNTIME) << "[update-check] current=" << PRIME_PORT_VERSION << " latest=" << tag
                           << " state=" << static_cast<int>(status.state) << std::endl;
    Set(status);
    g_running.store(false);
}

bool Enabled() {
    return RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeCheckUpdates, "WII_COMPILED_PRIME_CHECK_UPDATES", true);
}

} // namespace

void Start() {
    if (!Enabled()) {
        Status status; status.state = State::Disabled; status.message = "disabled in Config.toml ([prime] check_updates)";
        Set(status);
        return;
    }
    CheckNow();
}

void CheckNow() {
    if (g_running.exchange(true)) return;
    std::thread(Worker).detach();
}

Status Get() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

void OpenDownloadPage() {
#if defined(_WIN32)
    const Status status = Get();
    const std::string url = status.downloadUrl.empty() ? std::string(PRIME_PORT_REPOSITORY) + "/releases" : status.downloadUrl;
    ::ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const Status status = Get();
    const std::string url = status.downloadUrl.empty() ? std::string(PRIME_PORT_REPOSITORY) + "/releases" : status.downloadUrl;
    SDL_OpenURL(url.c_str());  // xdg-open: the default browser
#endif
}

} // namespace UpdateCheck
