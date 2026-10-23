#include <cstdint>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "nod.h"

namespace fs = std::filesystem;

static bool compare_blob(const NodBlob& blob, const fs::path& reference,
                         const char* label) {
    std::ifstream input(reference, std::ios::binary | std::ios::ate);
    if (!input) {
        std::cerr << "cannot open reference " << label << ": " << reference << '\n';
        return false;
    }
    const auto size = input.tellg();
    if (size < 0 || static_cast<uint64_t>(size) != blob.size) {
        std::cerr << label << " size mismatch: ISO=" << blob.size
                  << " reference=" << size << '\n';
        return false;
    }
    input.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input || !std::equal(bytes.begin(), bytes.end(), blob.data)) {
        std::cerr << label << " differs from the private research extraction\n";
        return false;
    }
    std::cout << label << " bytes=" << blob.size << " exact_match=true\n";
    return true;
}

static bool compare_file(NodHandle* partition, const char* path,
                         const fs::path& reference) {
    NodNodeKind kind = NOD_NODE_KIND_DIRECTORY;
    uint32_t size = 0;
    const uint32_t index = nod_partition_find_file(partition, path, &kind, &size);
    if (index == NOD_FST_STOP || kind != NOD_NODE_KIND_FILE) {
        std::cerr << "FST file not found: " << path << ": "
                  << (nod_error_message() ? nod_error_message() : "unknown error") << '\n';
        return false;
    }

    NodHandle* file = nullptr;
    if (nod_partition_open_file(partition, index, &file) != NOD_RESULT_OK) {
        std::cerr << "cannot open FST index " << index << ": "
                  << (nod_error_message() ? nod_error_message() : "unknown error") << '\n';
        return false;
    }
    std::ifstream input(reference, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || static_cast<uint64_t>(input.tellg()) != size) {
        std::cerr << "reference size mismatch for " << path << '\n';
        nod_free(file);
        return false;
    }
    input.seekg(0);
    std::vector<uint8_t> expected(size);
    input.read(reinterpret_cast<char*>(expected.data()), static_cast<std::streamsize>(size));
    if (!input) {
        std::cerr << "cannot read reference for " << path << '\n';
        nod_free(file);
        return false;
    }

    std::vector<uint8_t> actual(size);
    size_t offset = 0;
    while (offset < actual.size()) {
        const int64_t count = nod_read(file, actual.data() + offset, actual.size() - offset);
        if (count <= 0) {
            std::cerr << "short/error read for " << path << " at " << offset << '\n';
            nod_free(file);
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    nod_free(file);
    if (actual != expected) {
        std::cerr << "ISO file differs from the private research extraction: " << path << '\n';
        return false;
    }
    std::cout << "file=" << path << " bytes=" << size << " exact_match=true\n";
    return true;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: nod_iso_partition_probe.exe <disc.iso> <research-extraction>\n";
        return 2;
    }

    NodHandle* disc = nullptr;
    if (nod_disc_open(argv[1], nullptr, &disc) != NOD_RESULT_OK) {
        std::cerr << "cannot open disc: "
                  << (nod_error_message() ? nod_error_message() : "unknown error") << '\n';
        return 1;
    }
    NodHandle* partition = nullptr;
    NodPartitionOptions partitionOptions{};
    partitionOptions.validate_hashes = true;
    if (nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA,
                                     &partitionOptions, &partition) != NOD_RESULT_OK) {
        std::cerr << "cannot open data partition: "
                  << (nod_error_message() ? nod_error_message() : "unknown error") << '\n';
        nod_free(disc);
        return 1;
    }

    NodDiscHeader header{};
    if (nod_disc_header(disc, &header) != NOD_RESULT_OK ||
        std::memcmp(header.game_id, "RM3E01", 6) != 0 ||
        header.disc_num != 0 || header.disc_version != 0 ||
        header.wii_magic[0] != 0x5D || header.wii_magic[1] != 0x1C ||
        header.wii_magic[2] != 0x9E || header.wii_magic[3] != 0xA3) {
        std::cerr << "disc header is not RM3E01 disc 0 revision 0 Wii format\n";
        nod_free(partition);
        nod_free(disc);
        return 1;
    }

    NodPartitionMeta meta{};
    const fs::path extraction(argv[2]);
    bool ok = nod_partition_meta(partition, &meta) == NOD_RESULT_OK;
    if (!ok) {
        std::cerr << "cannot read partition metadata: "
                  << (nod_error_message() ? nod_error_message() : "unknown error") << '\n';
    } else {
        ok = compare_blob(meta.raw_dol, extraction / "sys" / "main.dol", "main.dol") &&
             compare_blob(meta.raw_fst, extraction / "sys" / "fst.bin", "fst.bin") &&
             compare_file(partition, "/FrontEnd.pak",
                          extraction / "files" / "FrontEnd.pak") &&
             compare_file(partition, "/GuiDVD.pak",
                          extraction / "files" / "GuiDVD.pak") &&
             compare_file(partition, "/RSO/Rev2 Production/selfile.sel",
                          extraction / "files" / "RSO" / "Rev2 Production" / "selfile.sel") &&
             compare_file(partition,
                          "/RSO/Rev2 Production/RSO_ScriptGUIScreenFrontEndUI.rso",
                          extraction / "files" / "RSO" / "Rev2 Production" /
                              "RSO_ScriptGUIScreenFrontEndUI.rso");
    }
    nod_free(partition);
    nod_free(disc);
    if (!ok) {
        return 1;
    }
    std::cout << "disc_id=RM3E01 disc=0 revision=0\n"
              << "direct_partition_read=true; partition_hash_validation=true; "
              << "source_iso_modified=false\n";
    return 0;
}
