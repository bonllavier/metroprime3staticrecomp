#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <system_error>

// Filesystem measurements for IOS GETUSAGE. Protocol facts were checked
// against the SDK caller and the pinned independent reference recorded in
// recomp/checkpoints/20261021_dolphin_fs_reference.json. No reference code
// is incorporated here. This header never reads or writes game resources.
namespace NandDirectoryUsage {
inline constexpr uint32_t kClusterBytes = 16384;
inline constexpr uint32_t kUsableClusters = 0x7BC0;
inline constexpr uint32_t kTotalInodes = 0x17FF;
enum class Error { None, Missing, Invalid, Io };
struct Result {
    Error error = Error::None;
    uint32_t clusters = 0;
    uint32_t inodes = 0;
};

inline bool ValidVectorSizes(uint32_t inputs, uint32_t outputs,
                             uint32_t pathBytes, uint32_t clusterBytes,
                             uint32_t inodeBytes) {
    return inputs == 1 && outputs == 2 && pathBytes == 64 &&
        clusterBytes == 4 && inodeBytes == 4;
}

inline Result Measure(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    if (directory.empty()) return {Error::Invalid};
    std::error_code error;
    const auto initial = fs::symlink_status(directory, error);
    if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(initial)))
        return {Error::Missing};
    if (error) return {Error::Io};
    if (!fs::is_directory(initial)) return {Error::Invalid};

    // The requested directory itself occupies an inode. Each regular file
    // occupies ceil(size/16KiB) clusters, including zero clusters when empty.
    uint64_t clusters = 0;
    uint64_t inodes = 1;
    fs::recursive_directory_iterator cursor(directory, fs::directory_options::none, error);
    const fs::recursive_directory_iterator end;
    if (error) return {Error::Io};
    while (cursor != end) {
        // Match the existing NAND directory adapter: host-only marker and
        // temporary names longer than the Wii's 12-byte limit are invisible.
        const auto name = cursor->path().filename().u8string();
        if (name.empty() || name.size() > 12) {
            cursor.disable_recursion_pending();
        } else {
            const auto status = cursor->symlink_status(error);
            if (error || fs::is_symlink(status)) return {Error::Io};
            if (fs::is_regular_file(status)) {
                const auto bytes = cursor->file_size(error);
                if (error) return {Error::Io};
                clusters += bytes / kClusterBytes + (bytes % kClusterBytes != 0);
                ++inodes;
            } else if (fs::is_directory(status)) {
                ++inodes;
            } else {
                return {Error::Io};
            }
        }
        cursor.increment(error);
        if (error) return {Error::Io};
    }
    // The IOS structure represents the fixed-size Wii NAND, even when its
    // managed host directory has grown beyond that capacity.
    return {Error::None,
        static_cast<uint32_t>(std::min<uint64_t>(clusters, kUsableClusters)),
        static_cast<uint32_t>(std::min<uint64_t>(inodes, kTotalInodes))};
}
} // namespace NandDirectoryUsage
