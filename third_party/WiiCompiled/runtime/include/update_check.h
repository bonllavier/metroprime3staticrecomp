#pragma once

#include <string>

// New-release check against the project's GitHub Releases (one HTTPS GET to
// api.github.com/repos/<owner>/<repo>/releases/latest on a worker thread).
// Compares the latest tag (vX.Y.Z) with PRIME_PORT_VERSION. Shown in
// F1 > Info and as a short on-screen notice when a newer build exists.
// Disabled with [prime] check_updates = false (or WII_COMPILED_PRIME_CHECK_UPDATES=0).
namespace UpdateCheck {

enum class State { Disabled, Idle, Checking, UpToDate, UpdateAvailable, Failed };

struct Status {
    State state = State::Idle;
    std::string latestVersion;  // "0.2.0" when known
    std::string downloadUrl;    // release page
    std::string message;        // human-readable detail (error text, timestamps)
};

// Starts the automatic check if enabled; safe to call once at startup.
void Start();
// Forces a new check (F1 > Info > Check now).
void CheckNow();
Status Get();
// Opens the release page (or the repository) in the default browser.
void OpenDownloadPage();

} // namespace UpdateCheck
