#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Writer for the Wii system configuration file /shared2/sys/SYSCONF, which the
// SDK SC library reads for the TV aspect ratio, sound mode, language and other
// console settings. Layout: "SCv0", u16 entry count, u16 offsets[count + 1]
// (the last one is the end of the data), entries, zero fill, "SCed" in the
// last four bytes of the 16 KiB file. Every entry starts with
// (type << 5) | (name length - 1) followed by the name and its payload; array
// payloads carry their length minus one. Big-endian throughout.
namespace WiiSysconf {
enum class Type : uint8_t { BigArray = 1, SmallArray = 2, Byte = 3, Short = 4, Long = 5, LongLong = 6, Bool = 7 };

struct Entry {
    std::string name;
    Type type;
    std::vector<uint8_t> data;
};

inline Entry Byte(std::string name, uint8_t value) { return {std::move(name), Type::Byte, {value}}; }

constexpr size_t kFileSize = 0x4000;

inline std::vector<uint8_t> Build(const std::vector<Entry>& entries) {
    std::vector<uint8_t> file(kFileSize, 0);
    const auto put16 = [&](size_t at, size_t value) {
        file[at] = static_cast<uint8_t>(value >> 8);
        file[at + 1] = static_cast<uint8_t>(value);
    };
    std::memcpy(file.data(), "SCv0", 4);
    put16(4, entries.size());
    size_t pos = 6 + 2 * (entries.size() + 1);
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.name.empty() || entry.name.size() > 32) throw std::invalid_argument("SYSCONF entry name");
        put16(6 + 2 * i, pos);
        size_t need = 1 + entry.name.size() + entry.data.size() + 2;
        if (pos + need > kFileSize - 4) throw std::length_error("SYSCONF entries exceed the file");
        file[pos++] = static_cast<uint8_t>((static_cast<uint8_t>(entry.type) << 5) | (entry.name.size() - 1));
        std::memcpy(file.data() + pos, entry.name.data(), entry.name.size());
        pos += entry.name.size();
        if (entry.type == Type::BigArray) {
            put16(pos, entry.data.size() - 1);
            pos += 2;
        } else if (entry.type == Type::SmallArray) {
            file[pos++] = static_cast<uint8_t>(entry.data.size() - 1);
        }
        std::memcpy(file.data() + pos, entry.data.data(), entry.data.size());
        pos += entry.data.size();
    }
    put16(6 + 2 * entries.size(), pos);
    std::memcpy(file.data() + kFileSize - 4, "SCed", 4);
    return file;
}

// Value of a one-byte entry, or -1 when the file lacks it (round-trip checks).
inline int FindByte(const std::vector<uint8_t>& file, const std::string& name) {
    if (file.size() != kFileSize || std::memcmp(file.data(), "SCv0", 4) != 0) return -1;
    const size_t count = (size_t(file[4]) << 8) | file[5];
    for (size_t i = 0; i < count; ++i) {
        const size_t at = (size_t(file[6 + 2 * i]) << 8) | file[7 + 2 * i];
        if (at + 1 >= file.size()) return -1;
        const size_t length = (file[at] & 0x1F) + 1;
        if (static_cast<Type>(file[at] >> 5) != Type::Byte || at + 1 + length >= file.size()) continue;
        if (name.size() == length && std::memcmp(file.data() + at + 1, name.data(), length) == 0)
            return file[at + 1 + length];
    }
    return -1;
}
}  // namespace WiiSysconf
