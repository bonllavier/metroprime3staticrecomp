#pragma once
#include <cstdlib>
#include <string>

namespace aurora::diagnostics {
// Debug environment switches are launch-time options. Copy the value once:
// getenv takes the Windows CRT environment lock even when a switch is absent.
// Presence and an explicitly empty value remain distinguishable.
struct EnvironmentValue {
    bool present=false;
    std::string value;
    explicit EnvironmentValue(const char* name) {
        if(const auto* raw=std::getenv(name)){present=true;value=raw;}
    }
    const char* get() const {return present ? value.c_str() : nullptr;}
};
}
// Public builds carry none of these switches: the code behind them compiles away.
#if !defined(MKW_PRIME_QA_TOOLS)
#define AURORA_DIAGNOSTIC_ENV(name) (static_cast<const char*>(nullptr))
#else
#define AURORA_DIAGNOSTIC_ENV(name) ([]() -> const char* { \
    static const aurora::diagnostics::EnvironmentValue value(name); \
    return value.get(); \
}())
#endif
