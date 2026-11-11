// See include/host_https.h. Non-Windows hosts only.
#if !defined(_WIN32)
#include "host_https.h"

#include <dlfcn.h>

#include <cstddef>
#include <mutex>
#include <type_traits>

namespace HostHttps {
namespace {

// The few libcurl entry points used, with the stable ABI constants from curl.h
// (CURLOPT_* = type base + number; these values have not changed since 7.x).
using CURL = void;
struct curl_slist;
using CurlCode = int;
constexpr int kOptUrl = 10002, kOptPostFields = 10015, kOptPostFieldSize = 60, kOptHttpHeader = 10023,
              kOptUserAgent = 10018, kOptWriteFunction = 20011, kOptWriteData = 10001, kOptTimeout = 13,
              kOptConnectTimeout = 78, kOptFollowLocation = 52, kOptNoSignal = 99, kOptProtocolsStr = 10318,
              kOptRedirProtocolsStr = 10319, kOptProtocols = 181, kOptRedirProtocols = 182;
constexpr long kProtoHttps = 1L << 1;  // CURLPROTO_HTTPS
constexpr int kInfoResponseCode = 0x200002;  // CURLINFO_LONG + 2
constexpr long kGlobalDefault = 3;           // CURL_GLOBAL_SSL | CURL_GLOBAL_WIN32

struct Api {
    CurlCode (*global_init)(long) = nullptr;
    CURL* (*easy_init)() = nullptr;
    CurlCode (*easy_setopt)(CURL*, int, ...) = nullptr;
    CurlCode (*easy_perform)(CURL*) = nullptr;
    CurlCode (*easy_getinfo)(CURL*, int, ...) = nullptr;
    void (*easy_cleanup)(CURL*) = nullptr;
    const char* (*easy_strerror)(CurlCode) = nullptr;
    curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
    void (*slist_free_all)(curl_slist*) = nullptr;
    bool ready = false;
};

const Api& Load() {
    static Api api;
    static std::once_flag once;
    std::call_once(once, [] {
        void* lib = nullptr;
        for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"})
            if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL)) != nullptr) break;
        if (!lib) return;
        const auto bind = [lib](auto& fn, const char* symbol) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, symbol));
            return fn != nullptr;
        };
        api.ready = bind(api.global_init, "curl_global_init") && bind(api.easy_init, "curl_easy_init") &&
                    bind(api.easy_setopt, "curl_easy_setopt") && bind(api.easy_perform, "curl_easy_perform") &&
                    bind(api.easy_getinfo, "curl_easy_getinfo") && bind(api.easy_cleanup, "curl_easy_cleanup") &&
                    bind(api.easy_strerror, "curl_easy_strerror") && bind(api.slist_append, "curl_slist_append") &&
                    bind(api.slist_free_all, "curl_slist_free_all");
        if (api.ready) api.ready = api.global_init(kGlobalDefault) == 0;
    });
    return api;
}

size_t Collect(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

}  // namespace

bool Available() { return Load().ready; }

Response Request(const std::string& url, const std::string* post, const std::vector<std::string>& headers,
                 const std::string& userAgent, long timeoutSeconds) {
    Response response;
    if (url.rfind("https://", 0) != 0) {
        response.error = "only https:// URLs are allowed";
        return response;
    }
    const Api& api = Load();
    if (!api.ready) {
        response.error = "libcurl (libcurl.so.4) is not installed";
        return response;
    }
    CURL* curl = api.easy_init();
    if (!curl) {
        response.error = "curl_easy_init failed";
        return response;
    }
    curl_slist* list = nullptr;
    for (const auto& header : headers) list = api.slist_append(list, header.c_str());
    api.easy_setopt(curl, kOptUrl, url.c_str());
    // Never downgrade to plain HTTP, redirects included. The *_STR options exist
    // since curl 7.85; older releases (Ubuntu 22.04 ships 7.81) take the bitmask.
    if (api.easy_setopt(curl, kOptProtocolsStr, "https") != 0) api.easy_setopt(curl, kOptProtocols, kProtoHttps);
    if (api.easy_setopt(curl, kOptRedirProtocolsStr, "https") != 0) api.easy_setopt(curl, kOptRedirProtocols, kProtoHttps);
    api.easy_setopt(curl, kOptUserAgent, userAgent.c_str());
    api.easy_setopt(curl, kOptNoSignal, 1L);  // called from worker threads
    api.easy_setopt(curl, kOptFollowLocation, 1L);
    api.easy_setopt(curl, kOptConnectTimeout, timeoutSeconds);
    api.easy_setopt(curl, kOptTimeout, timeoutSeconds);
    if (list) api.easy_setopt(curl, kOptHttpHeader, list);
    if (post) {
        api.easy_setopt(curl, kOptPostFields, post->data());
        api.easy_setopt(curl, kOptPostFieldSize, static_cast<long>(post->size()));
    }
    api.easy_setopt(curl, kOptWriteFunction, &Collect);
    api.easy_setopt(curl, kOptWriteData, &response.body);
    const CurlCode code = api.easy_perform(curl);
    if (code == 0) {
        api.easy_getinfo(curl, kInfoResponseCode, &response.status);
        response.ok = true;
    } else {
        response.error = api.easy_strerror(code);
    }
    if (list) api.slist_free_all(list);
    api.easy_cleanup(curl);
    return response;
}

}  // namespace HostHttps
#endif
