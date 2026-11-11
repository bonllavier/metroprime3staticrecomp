// Extracts the private build-time inputs of the RM3E01_00 static recompilation
// (sys/main.dol, sys/fst.bin and selected FST directories) directly from the
// player's disc image through nod, with partition hash validation. The image is
// only opened for reading. The published game keeps reading the image at run
// time; this tree is a build input only and is never distributed.
//
// Usage: nod_extract_build_inputs <disc.iso|.rvz|...> <out-dir> [fst-prefix ...]
//   e.g. nod_extract_build_inputs game.rvz extraction "RSO/Rev2 Production/"
// Writes <out-dir>/sys/{main.dol,fst.bin} and <out-dir>/files/<path> per match.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "nod.h"

namespace fs = std::filesystem;

namespace {

struct Entry {
    uint32_t index;
    std::string path;
    uint32_t size;
};

struct Walk {
    std::vector<std::pair<uint32_t, std::string>> dirs;  // (child end index, path prefix)
    std::vector<std::string> prefixes;
    std::vector<Entry> files;
};

uint32_t VisitNode(uint32_t index, NodNodeKind kind, const char* name, uint32_t size, void* user) {
    auto& walk = *static_cast<Walk*>(user);
    while (!walk.dirs.empty() && index >= walk.dirs.back().first) walk.dirs.pop_back();
    const std::string parent = walk.dirs.empty() ? std::string() : walk.dirs.back().second;
    if (index == 0) {  // root
        walk.dirs.emplace_back(size, std::string());
        return index + 1;
    }
    const std::string path = parent + name;
    if (kind == NOD_NODE_KIND_DIRECTORY) {
        walk.dirs.emplace_back(size, path + "/");
    } else {
        for (const auto& prefix : walk.prefixes) {
            if (path.rfind(prefix, 0) == 0) {
                walk.files.push_back({index, path, size});
                break;
            }
        }
    }
    return index + 1;
}

bool WriteBytes(const fs::path& path, const uint8_t* data, size_t size) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

const char* LastError() {
    const char* message = nod_error_message();
    return message ? message : "unknown error";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <disc image> <out-dir> [fst-prefix ...]\n", argv[0]);
        return 2;
    }
    const fs::path out(argv[2]);

    NodHandle* disc = nullptr;
    if (nod_disc_open(argv[1], nullptr, &disc) != NOD_RESULT_OK) {
        std::fprintf(stderr, "cannot open disc: %s\n", LastError());
        return 1;
    }
    NodDiscHeader header{};
    if (nod_disc_header(disc, &header) != NOD_RESULT_OK || std::memcmp(header.game_id, "RM3E01", 6) != 0 ||
        header.disc_num != 0 || header.disc_version != 0) {
        std::fprintf(stderr, "disc is not RM3E01 disc 0 revision 0\n");
        nod_free(disc);
        return 1;
    }
    NodPartitionOptions options{};
    options.validate_hashes = true;
    NodHandle* partition = nullptr;
    if (nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, &options, &partition) != NOD_RESULT_OK) {
        std::fprintf(stderr, "cannot open data partition: %s\n", LastError());
        nod_free(disc);
        return 1;
    }

    int status = 0;
    NodPartitionMeta meta{};
    if (nod_partition_meta(partition, &meta) != NOD_RESULT_OK ||
        !WriteBytes(out / "sys" / "main.dol", meta.raw_dol.data, meta.raw_dol.size) ||
        !WriteBytes(out / "sys" / "fst.bin", meta.raw_fst.data, meta.raw_fst.size)) {
        std::fprintf(stderr, "cannot write partition metadata: %s\n", LastError());
        status = 1;
    }

    Walk walk;
    for (int i = 3; i < argc; ++i) walk.prefixes.emplace_back(argv[i]);
    if (status == 0 && !walk.prefixes.empty()) {
        nod_partition_iterate_fst(partition, VisitNode, &walk);
        std::vector<uint8_t> buffer;
        for (const auto& file : walk.files) {
            NodHandle* handle = nullptr;
            if (nod_partition_open_file(partition, file.index, &handle) != NOD_RESULT_OK) {
                std::fprintf(stderr, "cannot open %s: %s\n", file.path.c_str(), LastError());
                status = 1;
                break;
            }
            buffer.resize(file.size);
            size_t offset = 0;
            while (offset < buffer.size()) {
                const int64_t count = nod_read(handle, buffer.data() + offset, buffer.size() - offset);
                if (count <= 0) break;
                offset += static_cast<size_t>(count);
            }
            nod_free(handle);
            if (offset != buffer.size() || !WriteBytes(out / "files" / file.path, buffer.data(), buffer.size())) {
                std::fprintf(stderr, "short read or write for %s\n", file.path.c_str());
                status = 1;
                break;
            }
        }
        std::printf("files=%zu\n", walk.files.size());
    }
    std::printf("dol_bytes=%llu fst_bytes=%llu\n", static_cast<unsigned long long>(meta.raw_dol.size),
                static_cast<unsigned long long>(meta.raw_fst.size));
    nod_free(partition);
    nod_free(disc);
    return status;
}
