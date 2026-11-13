#pragma once

#include <cstdint>
#include <string>

// Localized text fixes for the USA discs (hle/prime_text_fixes.cpp): strings of the German, French,
// Spanish and Italian tables that were left in English or have typos, recognized by the FNV-1a 64 of
// their exact UTF-8 text and the language the game runs in.
namespace PrimeTextFixes {

// True when the game runs in German, French, Spanish or Italian ([prime] language); read once, like
// the game reads SYSCONF.
bool Enabled();

// Applies the corrections known for the string whose original text hashes to `hash`, in the current
// language.
// Returns false (and leaves `text` alone) when the string has none.
bool Apply(uint64_t hash, std::string& text);

}  // namespace PrimeTextFixes
