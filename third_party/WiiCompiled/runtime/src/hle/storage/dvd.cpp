#include "hle_stubs.h"
#if defined(__linux__)
#include <aurora/host_cores.h>
#endif
#include "isa/big_endian.h"
#include "hle/dvd_contract.h"
#include "hle/prime_performance.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"

// Defined in hle/gx/gx_objects.cpp. DVD reads are DMA-class writes to guest
// RAM: the game brackets them with DCInvalidateRange (not a flush), so the GX
// layer's caches must be notified explicitly that these bytes changed.
extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);
#include "hle/storage/riivolution.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "hle/prime_disc_picker.h"
#include <algorithm>
#include <iterator>
#include <vector>
#include <string>
#include <map>
#include <unordered_set>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <mutex>
#include <thread>
#include <deque>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <sstream>
#include <utility>
#if defined(MKW_PRIME_NOD_AVAILABLE)
#include "nod.h"

void PrimeStartTrilogy();  // hle/os/prime_host_restart.cpp

namespace PrimeIsoRuntime {
// Which check failed. NoImage goes straight to the file dialog (discussion #5);
// the others show their message first.
enum class Failure { NoImage, BadPath, Unreadable, WrongDisc, Other };
struct DiscError : std::runtime_error {
    Failure failure;
    DiscError(Failure kind, const std::string& text) : std::runtime_error(text), failure(kind) {}
};

// The ID this executable runs (make_title_tree.py swaps it for R3ME01).
constexpr const char kExpectedDiscId[] = "RM3E01";

std::string NoImageText() {
    return "No Metroid Prime 3: Corruption disc image was found.\n\n"
           "Copy your own dump of the game (USA, RM3E01: .iso, .rvz, .wbfs, .gcz, .wia or .ciso) into the "
           "'game' folder next to the executable:\n  " +
           RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::PrimeGameImageDirectory()) +
           "\n\nor set [paths] iso_path in UserData/Config.toml to its location, or start the game with "
           "--iso \"<path>\" (or the CORRUPTION3_ISO environment variable).";
}

// Opens `utf8` with nod and checks its header; on success `disc` owns the handle.
// `trilogyHandoff` reports a Trilogy USA disc instead of failing on it.
void OpenAndCheck(const std::filesystem::path& path, NodHandle*& disc, bool* trilogyHandoff) {
    const auto utf8 = RuntimeConfigFile::PathToUtf8(path);
    if (path.empty()) throw DiscError(Failure::NoImage, NoImageText());
    if (const auto problem = PrimeDiscPath::Check(path, utf8); problem != PrimeDiscPath::Problem::None) {
        throw DiscError(Failure::BadPath, "The disc image path cannot be used:\n  " + utf8 + "\n\n" + PrimeDiscPath::ProblemText(problem));
    }
    // Preloader threads decrypt the following sector groups ahead of the
    // read position; room streaming reads files sequentially, and without
    // them every group was decrypted on the guest thread when first read.
    NodDiscOptions discOptions{};
    discOptions.preloader_threads = 2;
    if (nod_disc_open(utf8.c_str(), &discOptions, &disc) != NOD_RESULT_OK) {
        disc = nullptr;
        throw DiscError(Failure::Unreadable, "The file exists but could not be read as a Wii disc image (read-only):\n  " + utf8 +
                                                 "\n\nIt must be a complete, unmodified dump in .iso, .rvz, .wbfs, .gcz, .wia or .ciso format.");
    }
    NodDiscHeader header{};
    if (nod_disc_header(disc, &header) != NOD_RESULT_OK) {
        throw DiscError(Failure::Unreadable, "The disc image header could not be read:\n  " + utf8);
    }
#if !defined(PRIME_TRILOGY_TITLE)
    if (!std::memcmp(header.game_id, "R3ME01", 6) && trilogyHandoff) {
        *trilogyHandoff = true;
        return;
    }
#endif
    if (std::memcmp(header.game_id, "RM3E01", 6) || header.disc_num || header.disc_version) {
        const std::string found(reinterpret_cast<const char*>(header.game_id), 6);
        throw DiscError(Failure::WrongDisc, "The disc image is not Metroid Prime 3: Corruption (USA), game ID RM3E01, disc 1, revision 0:\n  " + utf8 +
                                                "\n\n" + PrimeDiscPath::WrongDiscText(found, header.disc_num, header.disc_version, kExpectedDiscId));
    }
}

// A path chosen in the dialog during this start: tried first and saved to
// Config.toml only once it opens (a wrong pick is never stored).
std::filesystem::path g_pendingPick;

struct Reader {
    NodHandle* disc = nullptr;
    NodHandle* partition = nullptr;
    std::mutex mutex;
    ~Reader() { nod_free(partition); nod_free(disc); }
    void Open() {
        if (partition) return;
        RuntimeConfigFile::PrimeIsoSource source = RuntimeConfigFile::PrimeIsoSource::None;
        const auto path = g_pendingPick.empty() ? RuntimeConfigFile::ResolvedPrimeIsoPath(&source) : g_pendingPick;
        const auto utf8 = RuntimeConfigFile::PathToUtf8(path);
        bool trilogy = false;
        OpenAndCheck(path, disc, &trilogy);
        if (trilogy) {
            // The Trilogy executables read the same Config.toml.
            if (!g_pendingPick.empty()) RuntimeConfigFile::SetPrimeIsoPath(g_pendingPick);
            PrimeStartTrilogy();
        }
        // SHA-1 verification of every 32 KiB group dominated streaming loads on
        // the guest thread. Decryption is unchanged; set
        // WII_COMPILED_PRIME_VALIDATE_ISO_HASHES=1 to verify every read again.
        const char* validate = std::getenv("WII_COMPILED_PRIME_VALIDATE_ISO_HASHES");
        NodPartitionOptions options{};
        options.validate_hashes = validate && std::strcmp(validate, "1") == 0;
        if (nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, &options, &partition) != NOD_RESULT_OK)
            throw DiscError(Failure::Other, "Cannot open Prime ISO data partition");
        RT_LOG(RT_TAG_DVD) << "[prime-iso-open] id=RM3E01 disc=0 revision=0 readonly=1 validate_hashes="
                           << options.validate_hashes << " image=" << utf8 << " source="
                           << (g_pendingPick.empty() ? RuntimeConfigFile::PrimeIsoSourceName(source) : "dialog") << std::endl;
    }
    // The data partition's original fst.bin, decoded by nod from the image.
    // This is what the extracted research folder used to provide as sys/fst.bin.
    std::vector<uint8_t> FstBytes() {
        std::lock_guard<std::mutex> lock(mutex);
        Open();
        NodPartitionMeta meta{};
        if (nod_partition_meta(partition, &meta) != NOD_RESULT_OK || meta.raw_fst.data == nullptr || meta.raw_fst.size < 12)
            throw std::runtime_error("The disc image has no readable file system table (fst.bin).");
        return std::vector<uint8_t>(meta.raw_fst.data, meta.raw_fst.data + meta.raw_fst.size);
    }
    // Drops a half-opened image so the next Open() retries with a new path.
    void Reset() {
        std::lock_guard<std::mutex> lock(mutex);
        nod_free(partition); partition = nullptr;
        nod_free(disc); disc = nullptr;
    }
    std::vector<uint8_t> Read(uint64_t offset, uint32_t length) {
        std::lock_guard<std::mutex> lock(mutex);
        Open();
        if (offset > INT64_MAX - static_cast<uint64_t>(length) ||
            nod_seek(partition, static_cast<int64_t>(offset), 0) != static_cast<int64_t>(offset))
            throw std::runtime_error("Prime ISO partition seek failed");
        std::vector<uint8_t> bytes(length);
        size_t done = 0;
        while (done < bytes.size()) {
            const int64_t count = nod_read(partition, bytes.data() + done, bytes.size() - done);
            if (count <= 0) throw std::runtime_error("Prime ISO partition read/hash validation failed or ended early");
            done += static_cast<size_t>(count);
        }
        return bytes;
    }
};
Reader reader;
} // namespace PrimeIsoRuntime
#endif

std::string PrimeDiscPicker::CheckImage(const std::filesystem::path& path) {
#if defined(MKW_PRIME_NOD_AVAILABLE)
    NodHandle* disc = nullptr;
    try {
        bool trilogy = false;
        PrimeIsoRuntime::OpenAndCheck(path, disc, &trilogy);
        nod_free(disc);
        return {};
    } catch (const std::exception& error) {
        nod_free(disc);
        return error.what();
    }
#else
    (void)path;
    return {};
#endif
}

namespace fs = std::filesystem;

// ============================================================================
// Configuration
// ============================================================================

// The extracted ISO "DATA" folder. We map its "files" subfolder to the DVD
// root "/" and its "sys" subfolder to "/sys/". The root is user-owned input:
// it is never embedded into or copied by the public runtime.
static fs::path g_dvdRoot;
static std::once_flag g_dvdRootOnce;

static uint32_t CurrentDiscGameCode() {
    return RuntimeHle::CurrentGameCode(0x524D4350u); // RMCP
}

// ============================================================================
// DVD Constants & Structures
// ============================================================================

#define DVD_STATE_FATAL_ERROR  -1
#define DVD_STATE_END           0
#define DVD_STATE_BUSY          1
#define DVD_STATE_WAITING       2
#define DVD_STATE_COVER_CLOSED  3
#define DVD_STATE_NO_DISK       4
#define DVD_STATE_COVER_OPEN    5

// Offsets in DVD Command Block (OS standard)
#define DVD_CB_OFFSET_STATE       0x0C
#define DVD_CB_OFFSET_TRANSFERRED 0x20
#define DVD_FILEINFO_OFFSET_ADDR  0x30
#define DVD_FILEINFO_OFFSET_LEN   0x34

struct DVDFileEntry {
    fs::path hostPath;
    std::string dvdPath;  // Virtual Wii path (e.g., "/Race/Course.szs")
    uint32_t size;
    uint32_t discOffsetWords = 0;
    bool isDirectory = false;
};

struct FstFileEntry {
    uint32_t start;
    uint32_t end;
    uint32_t size;
    std::string dvdPath;
    fs::path hostPath;
};

// Global State
static std::vector<DVDFileEntry> g_fileEntries;
static std::map<std::string, int32_t> g_pathToEntry;
// Maps each published file's disc range back to its runtime entry.
struct PublishedExtent {
    uint64_t startBytes = 0;
    uint64_t endBytes = 0;
    int32_t entryIndex = -1;
};
static std::vector<PublishedExtent> g_publishedExtents;
static bool g_dvdInitialized = false;
static std::vector<FstFileEntry> g_fstFiles;
static bool g_fstLoaded = false;
static std::unordered_set<std::string> g_loggedReadErrors;

#if MKW_PRIME_QA_TOOLS
// The title sequence and attract demo stream their THP while they play; the QA
// intro taps (kpad.cpp) press A for as long as these reads keep arriving.
static std::atomic<int64_t> g_primeIntroVideoReadNs{INT64_MIN};
static void NotePrimeIntroVideoRead(const std::string& path) {
    if (path != "/Video/FrontEnd/Mp3_Title_Sequence.thp" && path != "/Video/FrontEnd/attract01.thp") return;
    g_primeIntroVideoReadNs.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                  std::memory_order_relaxed);
}
#endif
// Disc bytes read since startup; read by the F8 profiler timeline.
static std::atomic<uint64_t> g_primeDvdBytesRead{0};
uint64_t PrimeDvdBytesReadTotal() { return g_primeDvdBytesRead.load(std::memory_order_relaxed); }
#if MKW_PRIME_QA_TOOLS
// 0 = no intro video streamed yet, 1 = one is streaming, 2 = it stopped.
int PrimeIntroVideoState() {
    const int64_t last = g_primeIntroVideoReadNs.load(std::memory_order_relaxed);
    if (last == INT64_MIN) return 0;
    const auto age = std::chrono::steady_clock::now().time_since_epoch() - std::chrono::steady_clock::duration(last);
    return age < std::chrono::milliseconds(750) ? 1 : 2;
}
#endif
static constexpr int32_t kDvdFatalError = -3;

extern "C" void DVDInit_8015EA1C();

// This MEM2 region is reserved at startup for the published FST.
extern "C" uint32_t g_dvdFstReservedBase;
extern "C" uint32_t g_dvdFstReservedSize;

// Byte-wise copy into guest RAM plus the DMA notification the GX caches need.
static void CopyToGuestAsDma(uint32_t dest, const uint8_t* data, size_t size) {
    // A RAM destination outside every guarded executable range receives the
    // same bytes Write8 would store, in one copy. Anything else (MMIO, a range
    // spanning regions, guarded code) keeps the per-byte store path.
    bool copied = false;
    if (size != 0 && Memory::Contains(dest, size) && !RecompMod::HandleExecutableWrite(dest, size, 0)) {
        if (auto* host = Memory::GetPointer(dest, size)) {
            std::memcpy(host, data, size);
            copied = true;
        }
    }
    for (size_t i = 0; !copied && i < size; ++i) {
        Memory::Write8(dest + static_cast<uint32_t>(i), data[i]);
    }
    GxNotifyGuestRamDmaWrite(dest, static_cast<uint32_t>(size));
}

// Host path strings only ever leave this module as UTF-8 display text.
static std::string HostPathText(const fs::path& path) {
    return RuntimeConfigFile::PathToUtf8(path);
}

static bool IsDvdDataRoot(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec &&
           fs::is_directory(path / "files", ec) && !ec &&
           fs::is_regular_file(path / "sys" / "fst.bin", ec) && !ec;
}

// Single fatal idiom for this module: crash artifacts under `reason`, the same
// text in the popup, a non-zero exit code, and no second generic report from
// the atexit handler.
[[noreturn]] static void FailDvd(const char* reason, const char* category,
                                 const std::string& details) {
    RuntimeCrash::WriteCrashArtifacts(reason, details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup(category, details);
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

[[noreturn]] static void FailDvdRoot(const char* source, const fs::path& path = {}) {
    RT_LOGF(RT_TAG_DVD, "ERROR: %s", source);
    if (!path.empty()) {
        std::fprintf(stderr, ": %s", HostPathText(path).c_str());
    }
    std::fprintf(stderr,
                 "\n[dvd] Set [paths] dvd_root in Config.toml "
                 "to your extracted Mario Kart Wii DATA directory.\n");
    std::string details = source ? source : "The configured DVD root could not be opened.";
    if (!path.empty()) {
        details += "\n\nPath: ";
        details += HostPathText(path);
    }
    details += "\n\nSet [paths] dvd_root in Config.toml to the extracted "
               "Mario Kart Wii DATA directory.";
    FailDvd("dvd_root", "DVD data is unavailable", details);
}

static const fs::path& GetDvdRoot() {
    std::call_once(g_dvdRootOnce, []() {
        const fs::path path = RuntimeConfigFile::ResolvedDvdRoot();
        if (path.empty()) {
            FailDvdRoot("No DVD root is configured");
        }
        if (!IsDvdDataRoot(path)) {
            FailDvdRoot("Configured DVD root is not an extracted DATA directory", path);
        }
        g_dvdRoot = path;
    });

    return g_dvdRoot;
}

static std::string NormalizePath(const std::string& path);
static fs::path ResolveDvdMappedHostPath(const std::string& dvdPath, const fs::path& fallbackHostPath);

static void InvokeDvdCallback(uint32_t callbackPtr, int32_t result, uint32_t fileInfoPtr) {
    if (callbackPtr == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = static_cast<uint32_t>(result);
    cpu.gpr[4] = fileInfoPtr;
    InvokeIndirectCpu(callbackPtr, &cpu);
}

// Same shape as InvokeDvdCallback minus the command block: the DVDLow callbacks
// take only a result, so r4 is deliberately left untouched here.
static void InvokeDvdLowCallback(uint32_t callbackPtr, int32_t result) {
    if (callbackPtr == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = static_cast<uint32_t>(result);
    InvokeIndirectCpu(callbackPtr, &cpu);
}

static void ReportDvdReadError(const std::string& hostPath,
                               int64_t offset,
                               uint32_t length,
                               const char* reason) {
    const std::string key = hostPath + "\n" + std::to_string(offset);
    if (!g_loggedReadErrors.insert(key).second) {
        return;
    }

    RT_LOGF(RT_TAG_DVD,
                 "ERROR: DVD read failed: host=\"%s\" offset=%lld (0x%llx) "
                 "length=%u: %s\n",
                 hostPath.c_str(),
                 static_cast<long long>(offset),
                 static_cast<unsigned long long>(offset),
                 length,
                 reason ? reason : "unknown read error");
}

static int32_t DvdReadFatal(uint32_t fileInfoPtr,
                            const std::string& hostPath,
                            int64_t offset,
                            uint32_t length,
                            const char* reason) {
    ReportDvdReadError(hostPath, offset, length, reason);
    try {
        Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_STATE,
                        static_cast<uint32_t>(DVD_STATE_FATAL_ERROR));
        Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_TRANSFERRED, 0);
    } catch (const Memory::AccessViolation&) {
    }
    return kDvdFatalError;
}

static void RegisterFileEntry(std::string dvdPath, const fs::path& hostPath, uint32_t size);

// Parses an original disc FST. With `hostRoot` the entries map to the extracted
// folder's files; without it (disc-image mode) every file is registered with
// an empty host path and the disc image itself serves the reads.
static void LoadFstIndexFromBytes(const std::vector<uint8_t>& data, const fs::path* hostRoot) {
    if (data.size() < 12) {
        return;
    }
    const uint32_t entryCount = BigEndian::Read32(&data[8]);
    if (entryCount == 0 || entryCount > 0x10000) {
        return;
    }

    const size_t entriesSize = static_cast<size_t>(entryCount) * 12;
    if (entriesSize >= data.size()) {
        return;
    }

    const size_t stringBase = entriesSize;

    struct DirFrame {
        uint32_t endIndex;
        std::string path;
    };

    std::vector<DirFrame> stack;
    stack.push_back({entryCount, std::string()});

    g_fstFiles.clear();
    g_fstFiles.reserve(entryCount / 2);

    for (uint32_t i = 1; i < entryCount; ++i) {
        while (!stack.empty() && i >= stack.back().endIndex) {
            stack.pop_back();
        }
        if (stack.empty()) {
            break;
        }

        const size_t entryOff = static_cast<size_t>(i) * 12;
        const uint32_t nameWord = BigEndian::Read32(&data[entryOff]);
        const uint8_t type = static_cast<uint8_t>(nameWord >> 24);
        const uint32_t nameOffset = nameWord & 0x00FFFFFFu;
        if (stringBase + nameOffset >= data.size()) {
            break;
        }

        const char* namePtr = reinterpret_cast<const char*>(&data[stringBase + nameOffset]);
        std::string name(namePtr);

        if (type != 0) {
            const uint32_t nextIndex = BigEndian::Read32(&data[entryOff + 8]);
            std::string dirPath = stack.back().path;
            if (!dirPath.empty()) {
                dirPath += "/";
            }
            dirPath += name;
            stack.push_back({nextIndex, dirPath});
            continue;
        }

        const uint32_t fileOffset = BigEndian::Read32(&data[entryOff + 4]);
        const uint32_t fileSize = BigEndian::Read32(&data[entryOff + 8]);

        std::string relPath = stack.back().path;
        if (!relPath.empty()) {
            relPath += "/";
        }
        relPath += name;

        const uint64_t startBytes64 = static_cast<uint64_t>(fileOffset) * 4ull;
        if (startBytes64 > 0xFFFFFFFFu) {
            continue;
        }
        const uint32_t startBytes = static_cast<uint32_t>(startBytes64);
        const uint32_t endBytes = startBytes + fileSize;

        FstFileEntry entry;
        entry.start = startBytes;
        entry.end = endBytes;
        entry.size = fileSize;
        entry.dvdPath = "/" + relPath;
        if (hostRoot) {
            const fs::path baseHostPath = *hostRoot / "files" / fs::path(relPath);
            entry.hostPath = ResolveDvdMappedHostPath(entry.dvdPath, baseHostPath);
        } else {
            RegisterFileEntry(entry.dvdPath, fs::path{}, fileSize);
        }
        g_fstFiles.push_back(std::move(entry));
    }

    std::sort(g_fstFiles.begin(), g_fstFiles.end(),
              [](const FstFileEntry& a, const FstFileEntry& b) { return a.start < b.start; });

}

static void LoadFstIndex() {
    if (g_fstLoaded) {
        return;
    }
    g_fstLoaded = true;

    const fs::path root = GetDvdRoot();
    std::ifstream fstFile(root / "sys" / "fst.bin", std::ios::binary);
    if (!fstFile.is_open()) {
        return;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(fstFile)), std::istreambuf_iterator<char>());
    if (!fstFile && !fstFile.eof()) {
        return;
    }
    LoadFstIndexFromBytes(data, &root);
}

static const PublishedExtent* FindPublishedExtentForByteOffset(uint64_t offset) {
    if (g_publishedExtents.empty()) {
        return nullptr;
    }

    auto it = std::upper_bound(
        g_publishedExtents.begin(), g_publishedExtents.end(), offset,
        [](uint64_t value, const PublishedExtent& extent) { return value < extent.startBytes; });
    if (it == g_publishedExtents.begin()) {
        return nullptr;
    }
    --it;
    if (offset >= it->startBytes && offset < it->endBytes) {
        return &(*it);
    }
    return nullptr;
}

struct AbsReadResult {
    const DVDFileEntry* entry = nullptr;
    uint32_t fileOffset = 0;
    uint32_t readLength = 0;
};

static bool ResolveAbsRead(uint32_t offset, uint32_t length, AbsReadResult& out) {
    // The extent table is available after DVDInit publishes the FST.
    DVDInit_8015EA1C();
    const PublishedExtent* extent = FindPublishedExtentForByteOffset(offset);
    uint64_t effectiveOffset = offset;

    if (!extent) {
        // Accept both byte offsets and the FST's four-byte word offsets.
        const uint64_t scaledOffset = static_cast<uint64_t>(offset) * 4ull;
        extent = FindPublishedExtentForByteOffset(scaledOffset);
        if (extent) {
            effectiveOffset = scaledOffset;
        }
    }

    if (!extent) {
        return false;
    }

    const DVDFileEntry& entry = g_fileEntries[extent->entryIndex];
    const uint32_t fileOffset = static_cast<uint32_t>(effectiveOffset - extent->startBytes);
    uint32_t readLength = length;
    if (readLength == 0 || fileOffset + readLength > entry.size) {
        readLength = entry.size - fileOffset;
    }

    out.entry = &entry;
    out.fileOffset = fileOffset;
    out.readLength = readLength;
    return true;
}

// Helper: Normalize paths (Windows backslash -> forward slash, lowercase for lookup)
static std::string NormalizePath(const std::string& path) {
    return DvdFstContract::NormalizeLookupPath(path);
}

static void RegisterFileEntry(std::string dvdPath, const fs::path& hostPath, uint32_t size) {
    dvdPath = DvdFstContract::CanonicalizePath(dvdPath);

    DVDFileEntry fileEntry;
    fileEntry.hostPath = hostPath;
    fileEntry.dvdPath = dvdPath;
    fileEntry.size = size;

    const int32_t entryNum = static_cast<int32_t>(g_fileEntries.size());
    g_fileEntries.push_back(std::move(fileEntry));
    g_pathToEntry[NormalizePath(dvdPath)] = entryNum;
}

// True when a disc path already has an entry (vanilla disc or an earlier
// overlay registration). Uses the same canonicalization as RegisterFileEntry.
static bool DvdEntryExists(const std::string& dvdPath) {
    return g_pathToEntry.find(NormalizePath(DvdFstContract::CanonicalizePath(dvdPath))) !=
           g_pathToEntry.end();
}

// Walk `root` and hand every entry to `visit`. Uses the error_code iterator forms so an
// unreadable entry doesn't abort the rest of the overlay. `announceErrors` keeps the
// disc-index scan noisy and the by-name mapping quiet, matching each caller's old behavior.
template <typename Visit>
static void WalkDirectory(const fs::path& root, bool recursive, bool announceErrors, Visit&& visit) {
    std::error_code ec;

    if (!recursive) {
        for (const auto& entry : fs::directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            visit(entry);
        }
        return;
    }

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (announceErrors) {
            RT_LOG(RT_TAG_DVD) << "WARNING: cannot enumerate " << HostPathText(root) << ": "
                      << ec.message() << std::endl;
        }
        return;
    }

    const fs::recursive_directory_iterator end;
    while (it != end) {
        const fs::directory_entry entry = *it;

        it.increment(ec);
        if (ec) {
            if (announceErrors) {
                RT_LOG(RT_TAG_DVD) << "WARNING: stopped enumerating " << HostPathText(root) << ": "
                          << ec.message() << std::endl;
                return;
            }
            visit(entry);
            return;
        }

        visit(entry);
    }
}

// Helper: Scan directory and build file index.
// addNewFiles=false is Riivolution's create="false": only files already on
// the disc may be replaced, nothing new is added.
static void ScanDirectory(const fs::path& root, const std::string& virtualPrefix,
                          bool recursive = true, bool addNewFiles = true) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return;
    }

    const auto registerEntry = [&](const fs::directory_entry& entry) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            return;
        }

        const std::uintmax_t size = fs::file_size(entry.path(), entryEc);
        if (entryEc) {
            RT_LOG(RT_TAG_DVD) << "WARNING: skipping " << HostPathText(entry.path()) << ": "
                      << entryEc.message() << std::endl;
            return;
        }

        const fs::path relative = fs::relative(entry.path(), root, entryEc);
        if (entryEc) {
            return;
        }

        std::string prefix = virtualPrefix.empty() ? "/" : virtualPrefix;
        if (prefix.back() != '/' && prefix.back() != '\\') {
            prefix += "/";
        }
        std::string dvdPath = prefix + HostPathText(relative);
        if (!addNewFiles && !DvdEntryExists(dvdPath)) {
            return;
        }
        RegisterFileEntry(std::move(dvdPath), entry.path(), static_cast<uint32_t>(size));
    };

    WalkDirectory(root, recursive, /*announceErrors=*/true, registerEntry);
}

// Riivolution <folder> without a disc path: replace disc files that share a
// filename with a file in the external folder (Dolphin's
// FindFilenameNodeInFST behavior, applied across the whole index).
static void ApplyFolderByNameMapping(const RuntimeRiivolution::Mapping& mapping) {
    // Snapshot the current index: matches by filename may only target entries
    // that exist before this mapping, and RegisterFileEntry appends.
    std::map<std::string, std::vector<std::string>> discPathsByName;
    const size_t entryCount = g_fileEntries.size();
    for (size_t i = 0; i < entryCount; ++i) {
        const std::string& dvdPath = g_fileEntries[i].dvdPath;
        const size_t lastSlash = dvdPath.rfind('/');
        std::string name = lastSlash == std::string::npos ? dvdPath : dvdPath.substr(lastSlash + 1);
        RuntimeHle::LowerInPlace(name);
        if (!name.empty()) {
            discPathsByName[name].push_back(dvdPath);
        }
    }

    uint32_t replaced = 0;
    const auto applyEntry = [&](const fs::directory_entry& entry) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            return;
        }
        std::string name = HostPathText(entry.path().filename());
        RuntimeHle::LowerInPlace(name);
        const auto matches = discPathsByName.find(name);
        if (matches == discPathsByName.end()) {
            return;
        }
        const std::uintmax_t size = fs::file_size(entry.path(), entryEc);
        if (entryEc) {
            return;
        }
        for (const std::string& dvdPath : matches->second) {
            RegisterFileEntry(dvdPath, entry.path(), static_cast<uint32_t>(size));
            ++replaced;
        }
    };

    WalkDirectory(mapping.hostPath, mapping.recursive, /*announceErrors=*/false, applyEntry);

    RT_LOG(RT_TAG_DVD) << HostPathText(mapping.hostPath) << ": replaced " << replaced
              << " disc file(s) by filename" << std::endl;
}

static void ScanOverlayRoot(const RuntimeRiivolution::Overlay& overlay) {
    if (!overlay.patches) {
        // Fallback for mod roots that mirror the disc filesystem directly (not a
        // Riivolution pack, which wouldn't map anything useful this way).
        RT_LOG(RT_TAG_DVD) << HostPathText(overlay.root)
                  << ": no Riivolution XML found, treating the root as a disc-shaped overlay"
                  << std::endl;
        ScanDirectory(overlay.root, "/");
        return;
    }

    for (const auto& mapping : overlay.patches->mappings) {
        switch (mapping.kind) {
        case RuntimeRiivolution::Mapping::Kind::File: {
            if (!mapping.create && !DvdEntryExists(mapping.discPath)) {
                break;
            }
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(mapping.hostPath, ec);
            if (!ec) {
                RegisterFileEntry(mapping.discPath, mapping.hostPath, static_cast<uint32_t>(size));
            }
            break;
        }
        case RuntimeRiivolution::Mapping::Kind::Folder:
            ScanDirectory(mapping.hostPath, mapping.discPath, mapping.recursive, mapping.create);
            break;
        case RuntimeRiivolution::Mapping::Kind::FolderByName:
            ApplyFolderByNameMapping(mapping);
            break;
        }
    }
}

static fs::path ResolveDvdMappedHostPath(const std::string& dvdPath, const fs::path& fallbackHostPath) {
    const std::string normalized = NormalizePath(dvdPath);
    const auto it = g_pathToEntry.find(normalized);
    if (it != g_pathToEntry.end() && it->second >= 0 &&
        it->second < static_cast<int32_t>(g_fileEntries.size())) {
        return g_fileEntries[it->second].hostPath;
    }

    for (const auto& overlay : RuntimeRiivolution::Overlays()) {
        const fs::path candidate = overlay.root / fs::path(normalized.substr(1));
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }

    return fallbackHostPath;
}

[[noreturn]] static void FailRuntimeFst(const std::string& reason) {
    RT_LOGF(RT_TAG_DVD, "ERROR: cannot publish the runtime FST: %s\n", reason.c_str());
    FailDvd("dvd_fst", "DVD file table is unavailable", reason);
}

static void BuildAndPublishRuntimeFst() {
    // Preserve real disc offsets where a runtime entry replaces a file from the
    // extracted image. Created Riivolution paths have no physical disc extent;
    // they are opened by their shared FST/SDK entry number instead.
    for (const FstFileEntry& fstFile : g_fstFiles) {
        const auto mapped = g_pathToEntry.find(NormalizePath(fstFile.dvdPath));
        if (mapped == g_pathToEntry.end() || mapped->second < 0 ||
            mapped->second >= static_cast<int32_t>(g_fileEntries.size())) {
            continue;
        }
        g_fileEntries[mapped->second].discOffsetWords = fstFile.start / 4u;
    }

    // Give runtime-added files unique synthetic disc ranges so reads can find them.
    uint64_t nextFreeBytes = 0;
    for (const FstFileEntry& fstFile : g_fstFiles) {
        nextFreeBytes = std::max(nextFreeBytes, static_cast<uint64_t>(fstFile.end));
    }
    for (const DVDFileEntry& entry : g_fileEntries) {
        if (entry.discOffsetWords != 0) {
            nextFreeBytes = std::max(
                nextFreeBytes, static_cast<uint64_t>(entry.discOffsetWords) * UINT64_C(4) +
                                   static_cast<uint64_t>(entry.size));
        }
    }
    for (DVDFileEntry& entry : g_fileEntries) {
        if (entry.isDirectory || entry.discOffsetWords != 0) {
            continue;
        }
        nextFreeBytes = (nextFreeBytes + 31ull) & ~31ull;
        if (nextFreeBytes / 4ull > 0xFFFFFFFFull) {
            FailRuntimeFst("synthetic disc extents exceed the 32-bit FST word offset range");
        }
        entry.discOffsetWords = static_cast<uint32_t>(nextFreeBytes / 4ull);
        nextFreeBytes += std::max<uint32_t>(entry.size, 4u);
    }

    std::vector<DvdFstContract::RegisteredFile> registrations;
    registrations.reserve(g_fileEntries.size());
    for (const DVDFileEntry& entry : g_fileEntries) {
        registrations.push_back({entry.hostPath, entry.dvdPath, entry.size, entry.discOffsetWords});
    }

    DvdFstContract::Image image;
    try {
        image = DvdFstContract::BuildImage(registrations);
    } catch (const std::exception& error) {
        FailRuntimeFst(error.what());
    }

    std::vector<DVDFileEntry> indexedEntries;
    indexedEntries.reserve(image.entries.size());
    for (const DvdFstContract::IndexedEntry& entry : image.entries) {
        indexedEntries.push_back({entry.hostPath, entry.dvdPath, entry.size,
                                  entry.discOffsetWords, entry.isDirectory});
    }
    g_fileEntries = std::move(indexedEntries);
    g_pathToEntry = std::move(image.pathToEntry);

    g_publishedExtents.clear();
    g_publishedExtents.reserve(g_fileEntries.size());
    for (size_t i = 0; i < g_fileEntries.size(); ++i) {
        const DVDFileEntry& entry = g_fileEntries[i];
        if (entry.isDirectory || entry.discOffsetWords == 0) {
            continue;
        }
        const uint64_t startBytes = static_cast<uint64_t>(entry.discOffsetWords) * 4ull;
        // Give empty files a one-byte range so their handles still resolve.
        const uint64_t endBytes = startBytes + std::max<uint32_t>(entry.size, 1u);
        g_publishedExtents.push_back({startBytes, endBytes, static_cast<int32_t>(i)});
    }
    std::sort(g_publishedExtents.begin(), g_publishedExtents.end(),
              [](const PublishedExtent& a, const PublishedExtent& b) {
                  return a.startBytes < b.startBytes;
              });

    // Use the FST memory reserved before guest code can allocate over it.
    if (g_dvdFstReservedBase == 0 || image.bytes.size() > g_dvdFstReservedSize ||
        !Memory::Contains(g_dvdFstReservedBase, g_dvdFstReservedSize)) {
        std::ostringstream reason;
        reason << "The " << image.bytes.size() << "-byte runtime FST does not fit the "
               << g_dvdFstReservedSize << "-byte guest reservation at 0x" << std::hex
               << g_dvdFstReservedBase << ".";
        FailRuntimeFst(reason.str());
    }

    const uint32_t fstAddress = g_dvdFstReservedBase;
    for (size_t i = 0; i < image.bytes.size(); ++i) {
        Memory::Write8(fstAddress + static_cast<uint32_t>(i), image.bytes[i]);
    }

    Memory::Write32(0x80000038u, fstAddress);
    Memory::Write32(0x8000003Cu, static_cast<uint32_t>(image.bytes.size()));

    RT_LOG(RT_TAG_DVD) << "published " << image.entries.size() << " FST entries ("
              << image.bytes.size() << " bytes) at 0x" << std::hex << fstAddress
              << " inside the boot-time MEM2 reservation" << std::dec << std::endl;

    if (const char* dumpPath = std::getenv("MKW_DUMP_FST")) {
        std::ofstream dump(dumpPath, std::ios::binary);
        dump.write(reinterpret_cast<const char*>(image.bytes.data()),
                   static_cast<std::streamsize>(image.bytes.size()));
        RT_LOG(RT_TAG_DVD) << "dumped published FST to " << dumpPath << std::endl;
    }
}

#if defined(MKW_GAME_PROFILE_RM3E01_00)
static void PublishPrimeDiscFstBytes(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 12 || bytes.size() > g_dvdFstReservedSize) {
        FailRuntimeFst("the RM3E01_00 disc FST has an invalid size");
    }
    const uint32_t fstAddress = g_dvdFstReservedBase;
    if (fstAddress == 0 || !Memory::Contains(fstAddress, bytes.size())) {
        FailRuntimeFst("the RM3E01_00 disc FST does not fit the reserved guest range");
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        Memory::Write8(fstAddress + static_cast<uint32_t>(i), bytes[i]);
    }
    Memory::Write32(0x80000038u, fstAddress);
    Memory::Write32(0x8000003Cu, static_cast<uint32_t>(bytes.size()));
    RT_LOG(RT_TAG_DVD) << "published original RM3E01_00 FST ("
                       << (BigEndian::Read32(bytes.data() + 8)) << " entries, "
                       << bytes.size() << " bytes) at 0x" << std::hex << fstAddress
                       << std::dec << std::endl;
    if (const char* dumpPath = std::getenv("MKW_DUMP_FST")) {
        std::ofstream dump(dumpPath, std::ios::binary);
        dump.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

static void PublishPrimeDiscFst() {
    const fs::path fstPath = GetDvdRoot() / "sys" / "fst.bin";
    std::ifstream fstFile(fstPath, std::ios::binary);
    if (!fstFile) {
        FailRuntimeFst("the RM3E01_00 disc FST could not be opened");
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(fstFile)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12) {
        FailRuntimeFst("the RM3E01_00 disc FST could not be read completely");
    }
    PublishPrimeDiscFstBytes(bytes);
}
#endif

extern "C" const char* DVDResolveHostPathForTest(const char* dvdPath)
{
    static std::string resolved;
    resolved.clear();

    if (!dvdPath || dvdPath[0] == '\0') {
        return nullptr;
    }

    DVDInit_8015EA1C();
    const std::string normalized = NormalizePath(dvdPath);
    const auto it = g_pathToEntry.find(normalized);
    if (it == g_pathToEntry.end() ||
        it->second < 0 ||
        it->second >= static_cast<int32_t>(g_fileEntries.size()) ||
        g_fileEntries[it->second].isDirectory) {
        return nullptr;
    }

    resolved = HostPathText(g_fileEntries[it->second].hostPath);
    return resolved.c_str();
}

// ============================================================================
// High-Level DVD API
// ============================================================================

static void InitDvdWaitingQueues()
{
    constexpr uint32_t kQueueBase = 0x80343230;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t queue = kQueueBase + (i * 8);
        Memory::Write32(queue + 0, queue);
        Memory::Write32(queue + 4, queue);
    }
}

static void CompleteDvdCancelState()
{
    InitDvdWaitingQueues();

    // These are the SDK DVD globals used by DVDCancelAll/__DVDPrepareReset.
    // The actual drive work is HLE'd, so complete pending cancel/reset waits.
    Memory::Write32(0x80386664, 0); // Canceling
    Memory::Write32(0x80386668, 0); // ResumeFromHere
    Memory::Write32(0x80386670, 0); // PausingFlag
    Memory::Write32(0x8038667C, 1); // CancelAllSync complete
    Memory::Write32(0x803866A8, 1); // PrepareReset complete
    Memory::Write32(0x803866F0, 0); // executing command block
}

// Prime's DVD/FST setup is performed by translated DOL code during OSInit.
// Publish its guest FST before entry so that code can consume low-memory
// 0x80000038/0x8000003C using the original Prime implementation.
extern "C" void PrimePublishDvdFst()
{
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    if (g_dvdInitialized) return;

    // Research layout: an extracted DATA folder ([paths] dvd_root) provides the
    // FST and host files. Players only have the disc image: its data partition
    // supplies the same FST through nod and every read goes to the image.
    const fs::path configuredRoot = RuntimeConfigFile::ResolvedDvdRoot();
    if (!configuredRoot.empty() && IsDvdDataRoot(configuredRoot)) {
        const fs::path& rootPath = GetDvdRoot();
        ScanDirectory(rootPath / "files", "/");
        ScanDirectory(rootPath / "sys", "/sys/");
        LoadFstIndex();
        BuildAndPublishRuntimeFst();
        PublishPrimeDiscFst();
    } else {
#if defined(MKW_PRIME_NOD_AVAILABLE)
        // No image configured, or the configured one is wrong: ask the player
        // with the file dialog and retry. The pick is saved to Config.toml only
        // once it opens, so a wrong one is not asked about again at every start.
        std::vector<uint8_t> fst;
        for (;;) {
            try {
                fst = PrimeIsoRuntime::reader.FstBytes();
                break;
            } catch (const std::exception& error) {
                PrimeIsoRuntime::reader.Reset();
                const auto* disc = dynamic_cast<const PrimeIsoRuntime::DiscError*>(&error);
                const bool noImage = disc && disc->failure == PrimeIsoRuntime::Failure::NoImage;
                RT_LOG(RT_TAG_DVD) << "[prime-iso-check] failed kind="
                                   << (disc ? static_cast<int>(disc->failure) : -1) << ": " << error.what() << std::endl;
                if (RuntimeConfigFile::PrimeDiscDialogDisabled()) {
                    FailDvd("prime_disc_image", "the disc image check failed and CORRUPTION3_NO_DISC_DIALOG=1 disables the dialog", error.what());
                }
                // Nothing configured at all: straight to the dialog, whose title says what to pick.
                const fs::path chosen = PrimeDiscPicker::Prompt(noImage ? std::string_view{} : std::string_view(error.what()));
                if (chosen.empty()) {
                    FailDvd("prime_disc_image", "no Metroid Prime 3 disc image was selected", error.what());
                }
                // A failing --iso / CORRUPTION3_ISO gives way to the pick for this run and its relaunches.
                RuntimeConfigFile::ClearPrimeIsoOverride();
                PrimeIsoRuntime::g_pendingPick = chosen;
                RT_LOG(RT_TAG_DVD) << "[prime-iso-pick] trying " << RuntimeConfigFile::PathToUtf8(chosen) << std::endl;
            }
        }
        if (!PrimeIsoRuntime::g_pendingPick.empty()) {
            const bool saved = RuntimeConfigFile::SetPrimeIsoPath(PrimeIsoRuntime::g_pendingPick);
            RT_LOG(RT_TAG_DVD) << "[prime-iso-pick] " << (saved ? "saved" : "could not save") << " iso_path="
                               << RuntimeConfigFile::PathToUtf8(PrimeIsoRuntime::g_pendingPick) << std::endl;
        }
        g_fstLoaded = true;
        LoadFstIndexFromBytes(fst, nullptr);
        BuildAndPublishRuntimeFst();
        PublishPrimeDiscFstBytes(fst);
        RT_LOG(RT_TAG_DVD) << "[prime-iso-fst] " << g_fstFiles.size()
                           << " files indexed from the disc image (no extracted folder)" << std::endl;
#else
        FailDvdRoot("No DVD root is configured");
#endif
    }
    g_dvdInitialized = true;
#endif
}

// 0x8015EA1C -> DVDInit
extern "C" void DVDInit_8015EA1C()
{
    if (g_dvdInitialized) return;
    // The scan below builds g_fileEntries incrementally, so a re-entrant call
    // must not start a second scan on top of a half-built index and duplicate
    // every entry.
    static bool initializing = false;
    if (initializing) return;
    initializing = true;


    // 1. Initialize Global Flags (Emulate OS state)
    // These addresses are standard OS globals for DVD context
    Memory::Write8(0x80386724, 1);   // Contexts initialized
    Memory::Write8(0x80386725, 1);   // LowInit called
    Memory::Write32(0x80386720, 0);  // Current context index
    Memory::Write8(0x803866a0, 1);   // DVDInit called flag
    CompleteDvdCancelState();

    // 2. Initialize DVD Context structures (prevent crashes in callbacks)
    constexpr uint32_t kContextBase = 0x803434e0;
    constexpr uint32_t kMagicValue = 0xFEEBDAED;
    for (int i = 0; i < 4; i++) {
        uint32_t ctx = kContextBase + i * 0x20;
        Memory::Write32(ctx + 0x0C, kMagicValue);
        Memory::Write32(ctx + 0x10, i);
    }

    // 3. Set Low Memory Globals (The "Magic" Identification)
    // This tells the game "Yes, I am Mario Kart Wii"
    uint32_t diskHeader = 0x80000000;
    Memory::Write32(diskHeader + 0x00, CurrentDiscGameCode());
    Memory::Write16(diskHeader + 0x04, 0x3031);     // '01' (Maker)
    Memory::Write8(diskHeader + 0x06, 0x01);        // Disk #1
    // 4. Scan Files
    const fs::path& rootPath = GetDvdRoot();
    
    // Map "<dvd_root>/files" -> "/"
    ScanDirectory(rootPath / "files", "/");

    // Map "<dvd_root>/sys" -> "/sys/" (e.g. main.dol, bi2.bin)
    ScanDirectory(rootPath / "sys", "/sys/");

    const auto& overlays = RuntimeRiivolution::Overlays();
    if (overlays.empty() && RuntimeProduct::IsRetroRewind()) {
        RT_LOG(RT_TAG_DVD) << "WARNING: no Retro Rewind overlay root was found. "
                     "File replacements (menu archives, karts, drivers, courses) will not apply "
                     "and the game will look and play like the unmodded disc. Set "
                     "[paths] retro_rewind_root in Config.toml "
                     "with the RetroRewind6 folder."
                  << std::endl;
    }
    // RegisterFileEntry lets the last registration win, so apply the roots in
    // reverse discovery order: the explicitly configured root outranks the mod
    // manifest.
    const size_t vanillaEntryCount = g_fileEntries.size();
    for (auto overlay = overlays.rbegin(); overlay != overlays.rend(); ++overlay) {
        ScanOverlayRoot(*overlay);
    }
    RT_LOG(RT_TAG_DVD) << "disc index: " << vanillaEntryCount << " disc file(s), "
              << (g_fileEntries.size() - vanillaEntryCount) << " overlay registration(s) from "
              << overlays.size() << " root(s)" << std::endl;

    // Load FST mapping so real files keep their physical disc extents.
    LoadFstIndex();
    BuildAndPublishRuntimeFst();

    // Initialize the translated DVD filesystem so it can use the published FST.
    constexpr uint32_t kDvdFsInitAddress = 0x8015DF1C;
    if (TranslatedFunctionRegistry::FindByAddressPtr(kDvdFsInitAddress)) {
        InvokeIndirectCpu(kDvdFsInitAddress, &GetPersistentCpuContext());
    }

    g_dvdInitialized = true;
    initializing = false;
}
PPC_NATIVE_OVERRIDE_VOID(8015EA1C, DVDInit_8015EA1C, (), ());

// FST-only DVD functions run translated so mods can override them safely.

// 0x8015E834 -> DVDReadPrio
extern "C" int32_t DVDReadPrio_8015E834(uint32_t fileInfoPtr, uint32_t bufferPtr, int32_t length, int32_t offset, int32_t prio)
{
    (void)prio;

    uint32_t startWords = 0;
    try {
        startWords = Memory::Read32(fileInfoPtr + DVD_FILEINFO_OFFSET_ADDR);
    } catch (const Memory::AccessViolation&) {
        return DvdReadFatal(fileInfoPtr, "<invalid DVD file info>", offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "DVD file info is outside guest memory");
    }

    // Treat startAddr as an FST offset, including offsets inside a file.
    const uint64_t startBytes = static_cast<uint64_t>(startWords) * 4ull;
    const PublishedExtent* extent = FindPublishedExtentForByteOffset(startBytes);
    if (!extent) {
        return DvdReadFatal(fileInfoPtr, "<unresolved DVD file handle>", offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "DVD file info does not reference a published FST extent");
    }

    const DVDFileEntry& entry = g_fileEntries[extent->entryIndex];

    if (offset < 0 || length < 0) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "negative DVD read offset or length");
    }

    const uint64_t extentBias = startBytes - extent->startBytes;
    const uint64_t requestedOffset = extentBias + static_cast<uint32_t>(offset);
    uint32_t uLength = (uint32_t)length;

    if (requestedOffset >= entry.size) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset, uLength,
                            "read offset is outside the indexed DVD file");
    }
    const uint32_t uOffset = static_cast<uint32_t>(requestedOffset);

    if (uLength > entry.size - uOffset) {
        uLength = entry.size - uOffset;
    }

    if (uLength != 0 && !Memory::Contains(bufferPtr, uLength)) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset, uLength,
                            "DVD read destination is outside guest memory");
    }

    std::vector<uint8_t> tempBuf;
    DvdReadContract::HostReadFailure failure;
    if (!DvdReadContract::ReadExact(entry.hostPath, uOffset, uLength, tempBuf, failure)) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset, uLength,
                            DvdReadContract::Describe(failure));
    }

    CopyToGuestAsDma(bufferPtr, tempBuf.data(), uLength);

    Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_TRANSFERRED, uLength);
    Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END);
    CompleteDvdCancelState();

    return static_cast<int32_t>(uLength);
}
PPC_NATIVE_OVERRIDE(8015E834, DVDReadPrio_8015E834, int32_t, (uint32_t f, uint32_t b, int32_t l, int32_t o, int32_t p), (f, b, l, o, p));

// 0x8015E74C -> DVDReadAsyncPrio (internal)
// HLE: perform synchronous read and invoke callback immediately.
extern "C" int32_t DVD__ReadAsyncPrio_HLE_8015e74c(uint32_t fileInfoPtr,
                                                   uint32_t bufferPtr,
                                                   int32_t length,
                                                   int32_t offset,
                                                   uint32_t callbackPtr,
                                                   int32_t prio)
{
    const int32_t bytesRead = DVDReadPrio_8015E834(fileInfoPtr, bufferPtr, length, offset, prio);

    InvokeDvdCallback(callbackPtr, bytesRead, fileInfoPtr);
    CompleteDvdCancelState();
    return bytesRead >= 0 ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(8015E74C, DVD__ReadAsyncPrio_HLE_8015e74c, int32_t,
         (uint32_t f, uint32_t b, int32_t l, int32_t o, uint32_t cb, int32_t p),
         (f, b, l, o, cb, p));

// 0x801628CC -> DVDReadAbsAsyncPrio (internal)
// HLE: resolve the absolute disc offset to a host file and read it.
extern "C" int32_t DVD__ReadAbsAsyncPrio_HLE_801628cc(uint32_t cmdBlockPtr,
                                                      uint32_t bufferPtr,
                                                      int32_t length,
                                                      int32_t offset,
                                                      uint32_t callbackPtr,
                                                      int32_t prio)
{
    (void)prio;
    int32_t bytesRead = -1;
    {
        AbsReadResult readInfo;
        const uint32_t requestedLength = length > 0 ? static_cast<uint32_t>(length) : 0;
        const uint32_t absoluteOffset = static_cast<uint32_t>(offset);
        if (length < 0) {
            bytesRead = DvdReadFatal(cmdBlockPtr, "<unmapped DVD offset>", absoluteOffset, 0,
                                     "negative absolute DVD read length");
        } else if (!ResolveAbsRead(absoluteOffset, requestedLength, readInfo)) {
            bytesRead = DvdReadFatal(cmdBlockPtr, "<unmapped DVD offset>", absoluteOffset,
                                     requestedLength,
                                     "absolute DVD read offset is not mapped to a host file");
        } else if (requestedLength != 0 && readInfo.readLength != requestedLength) {
            bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                     readInfo.fileOffset, requestedLength,
                                     "requested range extends beyond the indexed DVD file");
        } else if (requestedLength != 0 && !Memory::Contains(bufferPtr, requestedLength)) {
            bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                     readInfo.fileOffset, requestedLength,
                                     "DVD read destination is outside guest memory");
        } else {
            std::vector<uint8_t> tempBuf;
            DvdReadContract::HostReadFailure failure;
            if (!DvdReadContract::ReadExact(readInfo.entry->hostPath,
                                            readInfo.fileOffset,
                                            readInfo.readLength,
                                            tempBuf,
                                            failure)) {
                bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                         readInfo.fileOffset, readInfo.readLength,
                                         DvdReadContract::Describe(failure));
            } else {
                CopyToGuestAsDma(bufferPtr, tempBuf.data(), tempBuf.size());
                bytesRead = static_cast<int32_t>(tempBuf.size());
            }
        }

        if (bytesRead >= 0) {
            try {
                Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END);
                Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_TRANSFERRED,
                                static_cast<uint32_t>(bytesRead));
            } catch (const Memory::AccessViolation&) {
            }
        }
    }

    InvokeDvdCallback(callbackPtr, bytesRead, cmdBlockPtr);
    CompleteDvdCancelState();
    return bytesRead >= 0 ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(801628CC, DVD__ReadAbsAsyncPrio_HLE_801628cc, int32_t,
         (uint32_t cb, uint32_t b, int32_t l, int32_t o, uint32_t cbfn, int32_t p),
         (cb, b, l, o, cbfn, p));


// ============================================================================
// Low-Level / Core DVD (The "Magic" Handlers)
// ============================================================================

// 0x80164848 -> DVDLowInit
extern "C" int32_t DVDLowInit_80164848() {
    // Just ensure init is done
    DVDInit_8015EA1C(); 
    return 1;
}
PPC_NATIVE_OVERRIDE(80164848, DVDLowInit_80164848, int32_t, (), ());

extern "C" int32_t DVDLowInquiry_80165A30(uint32_t cmdBlockPtr, uint32_t callback)
{
    // HLE: acknowledge the drive is present. Must mark the command block complete
    // (State = 0) or callers polling this address hang waiting for "Busy" to clear.
    if (cmdBlockPtr) {
        Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END);
    }
    CompleteDvdCancelState();

    return 1; // Return 1 to indicate the command was successfully issued.
}
PPC_NATIVE_OVERRIDE(80165A30, DVDLowInquiry_80165A30, int32_t, (uint32_t b, uint32_t c), (b, c));

// 0x80164AAC -> DVDLowReadDiskID
extern "C" int32_t DVDLowReadDiskID_80164AAC(uint32_t diskIdPtr, uint32_t callback) {
    if (diskIdPtr) {
        Memory::Write32(diskIdPtr + 0x00, CurrentDiscGameCode());
        Memory::Write16(diskIdPtr + 0x04, 0x3031);
        Memory::Write8 (diskIdPtr + 0x06, 0x01);
    }
    CompleteDvdCancelState();
    return 1; // Success
}
PPC_NATIVE_OVERRIDE(80164AAC, DVDLowReadDiskID_80164AAC, int32_t, (uint32_t p, uint32_t c), (p, c));

// 0x80166330 -> DVDLowRead (And 0x80165708 UnencryptedRead)
// The game calls this to read the Disk Header (offset 0) or raw data.
extern "C" int32_t DVDLowRead_80166330(uint32_t buffer, uint32_t length, uint32_t offset, uint32_t callback)
{
    const auto finish = [callback](bool succeeded) {
        const auto completion = DvdReadContract::CompletionFor(succeeded);
        InvokeDvdLowCallback(callback, completion.callbackResult);
        CompleteDvdCancelState();
        return completion.returnValue;
    };

    if (length == 0) {
        return finish(true);
    }

    if (offset == 0 && length >= 0x20) {
        if (!Memory::Contains(buffer, length)) {
            ReportDvdReadError("<synthetic disc header>", offset, length,
                               "DVD read destination is outside guest memory");
            return finish(false);
        }
        Memory::Write32(buffer + 0x00, CurrentDiscGameCode());
        Memory::Write16(buffer + 0x04, 0x3031);     // 01
        GxNotifyGuestRamDmaWrite(buffer, length);
        return finish(true);
    }

    AbsReadResult readInfo;
    if (!ResolveAbsRead(offset, length, readInfo)) {
        ReportDvdReadError("<unmapped DVD offset>", offset, length,
                           "absolute DVD read offset is not mapped to a host file");
        return finish(false);
    }
    if (readInfo.readLength != length) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset, length,
                           "requested range extends beyond the indexed DVD file");
        return finish(false);
    }
    if (!Memory::Contains(buffer, length)) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset, length,
                           "DVD read destination is outside guest memory");
        return finish(false);
    }

    std::vector<uint8_t> tempBuf;
    DvdReadContract::HostReadFailure failure;
    if (!DvdReadContract::ReadExact(readInfo.entry->hostPath,
                                    readInfo.fileOffset,
                                    readInfo.readLength,
                                    tempBuf,
                                    failure)) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset,
                           readInfo.readLength, DvdReadContract::Describe(failure));
        return finish(false);
    }

    CopyToGuestAsDma(buffer, tempBuf.data(), tempBuf.size());
    return finish(true);
}
PPC_NATIVE_OVERRIDE(80166330, DVDLowRead_80166330, int32_t, (uint32_t b, uint32_t l, uint32_t o, uint32_t c), (b, l, o, c));

// RM3E01 IOS 0x71 uses an exact decrypted-partition byte offset. Keep this
// separate from MKW's byte/word heuristic and synthetic disc-header shortcut.
// The pinned nod adapter reads the original partition, including alignment
// bytes outside file extents. The fallback build retains the research-file
// reader. FST publication/startup still depends on the research extraction.
struct PublishedExtent;
static void FinishPrimePartitionRead(uint32_t buffer, uint32_t length, uint64_t byteOffset,
                                     const PublishedExtent* extent, uint64_t fileOffset,
                                     const std::vector<uint8_t>& bytes);

extern "C" bool DVD_HLE_ReadPrimePartition(uint32_t buffer, uint32_t length, uint64_t byteOffset)
{
    PrimePerformance::Scope performance(PrimePerformance::IsoRead);
    DVDInit_8015EA1C();
    if (length == 0) return true;
    if (buffer == 0 || !Memory::Contains(buffer, length)) return false;
    const PublishedExtent* extent = FindPublishedExtentForByteOffset(byteOffset);
#if !defined(MKW_PRIME_NOD_AVAILABLE)
    if (extent == nullptr || byteOffset + length > extent->endBytes) {
        RT_LOG(RT_TAG_DVD) << "Prime partition range not backed by an original FST file: offset=0x"
                            << std::hex << byteOffset << " length=0x" << length << std::dec << std::endl;
        if (const CpuContext* cpu = TryGetCpuContext()) {
            RT_LOG(RT_TAG_DVD) << "[prime-read-rejected-context] lr=0x" << std::hex << cpu->lr
                << " sp=0x" << cpu->gpr[1] << " buffer=0x" << buffer << std::dec << std::endl;
            uint32_t stack = cpu->gpr[1];
            for (uint32_t level = 0; level < 16u && Memory::Contains(stack, 8u); ++level) {
                const uint32_t previous = Memory::Read32(stack);
                if (previous <= stack || !Memory::Contains(previous, 8u)) break;
                RT_LOG(RT_TAG_DVD) << "[prime-read-rejected-frame] sp=0x" << std::hex << stack
                    << " caller=0x" << Memory::Read32(previous + 4u) << std::dec << std::endl;
                stack = previous;
            }
        }
        return false;
    }
#endif
#if defined(MKW_PRIME_NOD_AVAILABLE)
    std::vector<uint8_t> bytes = PrimeIsoRuntime::reader.Read(byteOffset, length);
    const uint64_t fileOffset = extent ? byteOffset - extent->startBytes : 0u;
#else
    const DVDFileEntry& entry = g_fileEntries[extent->entryIndex];
    const uint64_t fileOffset = byteOffset - extent->startBytes;
    std::vector<uint8_t> bytes;
    DvdReadContract::HostReadFailure failure;
    if (!DvdReadContract::ReadExact(entry.hostPath, fileOffset, length, bytes, failure)) {
        ReportDvdReadError(HostPathText(entry.hostPath), fileOffset, length,
                           DvdReadContract::Describe(failure));
        return false;
    }
#endif
    FinishPrimePartitionRead(buffer, length, byteOffset, extent, fileOffset, bytes);
    return true;
}

// Guest thread: the DMA into guest RAM and everything that observes it (texture
// and display-list invalidation, intro detection). The disc read itself may have
// run on the disc worker.
static void FinishPrimePartitionRead(uint32_t buffer, uint32_t length, uint64_t byteOffset,
                                     const PublishedExtent* extent, uint64_t fileOffset,
                                     const std::vector<uint8_t>& bytes) {
    (void)byteOffset;
    CopyToGuestAsDma(buffer, bytes.data(), bytes.size());
#if MKW_PRIME_QA_TOOLS
    if (extent) NotePrimeIntroVideoRead(g_fileEntries[extent->entryIndex].dvdPath);
#endif
    g_primeDvdBytesRead.fetch_add(length, std::memory_order_relaxed);
    // Byte-by-byte readback and the per-read lines were bring-up diagnostics;
    // with ~100k reads per session they cost the game thread on every load.
    static std::atomic<uint32_t> seen;
    if (!RtLogRepeated(seen)) return;
    uint64_t fingerprint = UINT64_C(14695981039346656037);
    for (uint32_t i = 0; i < length; ++i) {
        const uint8_t byte = Memory::Read8(buffer + i);
        if (byte != bytes[i])
            throw Memory::AccessViolation(buffer + i, 1u, "Prime DVD DMA readback differs from source file");
        fingerprint = (fingerprint ^ byte) * UINT64_C(1099511628211);
    }
    RT_LOG(RT_TAG_DVD) << "[prime-partition-read] path=" << (extent ? g_fileEntries[extent->entryIndex].dvdPath : "<partition>")
                      << " offset=0x" << std::hex << fileOffset
                      << " length=0x" << length << " buffer=0x" << buffer
                      << " fnv1a64=0x" << fingerprint << std::dec << std::endl;
    RT_LOG(RT_TAG_DVD) << "[prime-partition-readback] buffer=0x" << std::hex << buffer
        << " length=0x" << length << " fnv1a64=0x" << fingerprint << std::dec << std::endl;
#if defined(MKW_PRIME_NOD_AVAILABLE)
    RT_LOG(RT_TAG_DVD) << "[prime-iso-read] offset=0x" << std::hex << byteOffset
        << " length=0x" << length << " buffer=0x" << buffer << " fnv1a64=0x" << fingerprint
        << std::dec << " hashes_validated=1" << std::endl;
#endif
}

#if defined(MKW_PRIME_NOD_AVAILABLE)
// Disc worker: the IOS DI read (nod seek, AES decryption, sector assembly) runs
// beside the game the way the drive DMA does; the guest-side copy and the IPC
// reply happen on the guest thread once the bytes are ready. One worker keeps
// replies in submission order.
namespace PrimeAsyncDisc {
struct Job {
    uint32_t request = 0, buffer = 0, length = 0;
    uint64_t offset = 0;
    std::chrono::steady_clock::time_point readyAt{};
    std::vector<uint8_t> bytes;
    bool ok = false;
};
std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Job> g_pending, g_done;
std::atomic<uint32_t> g_doneCount{0};
std::once_flag g_started;

void WorkerMain() {
#if defined(_WIN32)
    SetThreadDescription(GetCurrentThread(), L"Prime disc reads");
#elif defined(__linux__)
    aurora::host_cores::set_thread_name("Prime disc");
#endif
    for (;;) {
        Job job;
        {
            std::unique_lock lock(g_mutex);
            g_cv.wait(lock, [] { return !g_pending.empty(); });
            job = std::move(g_pending.front());
            g_pending.pop_front();
        }
        try {
            job.bytes = PrimeIsoRuntime::reader.Read(job.offset, job.length);
            job.ok = job.bytes.size() == job.length;
        } catch (const std::exception& error) {
            RT_LOG(RT_TAG_DVD) << "[prime-async-disc] read failed offset=0x" << std::hex << job.offset
                               << " length=0x" << job.length << std::dec << ": " << error.what() << std::endl;
        }
        {
            std::lock_guard lock(g_mutex);
            g_done.push_back(std::move(job));
        }
        g_doneCount.fetch_add(1, std::memory_order_release);
    }
}
} // namespace PrimeAsyncDisc
#endif

extern "C" bool DVD_HLE_SubmitPrimePartitionRead(uint32_t request, uint32_t buffer, uint32_t length,
                                                 uint64_t byteOffset, uint64_t minDelayMicros) {
#if defined(MKW_PRIME_NOD_AVAILABLE)
    if (!RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeAsyncDiscReads, "WII_COMPILED_PRIME_ASYNC_DISC_READS",
                                        true) ||
        length == 0) {
        return false;
    }
    DVDInit_8015EA1C();
    if (buffer == 0 || !Memory::Contains(buffer, length)) return false;
    std::call_once(PrimeAsyncDisc::g_started, [] { std::thread(PrimeAsyncDisc::WorkerMain).detach(); });
    PrimeAsyncDisc::Job job;
    job.request = request;
    job.buffer = buffer;
    job.length = length;
    job.offset = byteOffset;
    job.readyAt = std::chrono::steady_clock::now() + std::chrono::microseconds(minDelayMicros);
    {
        std::lock_guard lock(PrimeAsyncDisc::g_mutex);
        PrimeAsyncDisc::g_pending.push_back(std::move(job));
    }
    PrimeAsyncDisc::g_cv.notify_one();
    return true;
#else
    (void)request; (void)buffer; (void)length; (void)byteOffset; (void)minDelayMicros;
    return false;
#endif
}

extern "C" uint32_t DVD_HLE_PrimeCompletedReadCount() {
#if defined(MKW_PRIME_NOD_AVAILABLE)
    return PrimeAsyncDisc::g_doneCount.load(std::memory_order_acquire);
#else
    return 0;
#endif
}

// Guest thread: finishes the oldest completed read. Returns false when none is ready.
extern "C" bool DVD_HLE_TakeCompletedPrimeRead(uint32_t* request, bool* ok, uint64_t* remainingDelayMicros) {
#if defined(MKW_PRIME_NOD_AVAILABLE)
    PrimeAsyncDisc::Job job;
    {
        std::lock_guard lock(PrimeAsyncDisc::g_mutex);
        if (PrimeAsyncDisc::g_done.empty()) return false;
        job = std::move(PrimeAsyncDisc::g_done.front());
        PrimeAsyncDisc::g_done.pop_front();
    }
    PrimeAsyncDisc::g_doneCount.fetch_sub(1, std::memory_order_acq_rel);
    if (job.ok) {
        PrimePerformance::Scope performance(PrimePerformance::IsoRead);
        const PublishedExtent* extent = FindPublishedExtentForByteOffset(job.offset);
        const uint64_t fileOffset = extent ? job.offset - extent->startBytes : 0u;
        FinishPrimePartitionRead(job.buffer, job.length, job.offset, extent, fileOffset, job.bytes);
    }
    const auto now = std::chrono::steady_clock::now();
    *request = job.request;
    *ok = job.ok;
    *remainingDelayMicros = job.readyAt > now
        ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(job.readyAt - now).count())
        : 0u;
    return true;
#else
    (void)request; (void)ok; (void)remainingDelayMicros;
    return false;
#endif
}

// UnencryptedRead has the same completion and failure contract as DVDLowRead.
extern "C" int32_t DVDLowUnencryptedRead_80165708(uint32_t b, uint32_t l, uint32_t o, uint32_t c) {
    return DVDLowRead_80166330(b, l, o, c);
}
PPC_NATIVE_OVERRIDE(80165708, DVDLowUnencryptedRead_80165708, int32_t, (uint32_t b, uint32_t l, uint32_t o, uint32_t c), (b, l, o, c));

// ============================================================================
// Other Necessary Stubs
// ============================================================================

extern "C" int32_t DVDCheckDevice_801643FC() { return 1; } // Ready
PPC_NATIVE_OVERRIDE(801643FC, DVDCheckDevice_801643FC, int32_t, (), ());

extern "C" int32_t DVDLowClearCoverInterrupt_80166964(uint32_t cb) { return 1; }
PPC_NATIVE_OVERRIDE(80166964, DVDLowClearCoverInterrupt_80166964, int32_t, (uint32_t cb), (cb));

