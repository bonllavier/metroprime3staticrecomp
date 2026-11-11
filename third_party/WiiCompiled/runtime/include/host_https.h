#pragma once
// Minimal HTTPS client for non-Windows hosts (Windows uses WinHTTP directly).
// libcurl is loaded at run time (libcurl.so.4), so the build gains no
// dependency and the system's CA store and proxy settings apply. When the
// library is missing, requests fail with an explanatory error.
#include <string>
#include <vector>

namespace HostHttps {

struct Response {
    bool ok = false;      // transfer completed (any HTTP status)
    long status = 0;      // HTTP status code
    std::string body;
    std::string error;    // set when !ok
};

// GET when `post` is null, POST otherwise. `headers` are full "Name: value" lines.
Response Request(const std::string& url, const std::string* post, const std::vector<std::string>& headers,
                 const std::string& userAgent, long timeoutSeconds = 15);

bool Available();

}  // namespace HostHttps
