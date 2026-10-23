#include "nod.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

// Private research output only. Reads the decrypted partition, including bytes
// outside FST file extents, with Wii cluster hash validation enabled.
int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: nod_partition_range_probe.exe disc.iso byte_offset size private-output.bin\n";
        return 2;
    }
    NodHandle* disc = nullptr;
    NodHandle* partition = nullptr;
    int result = 1;
    try {
        const uint64_t offset = std::stoull(argv[2], nullptr, 0);
        const uint64_t length = std::stoull(argv[3], nullptr, 0);
        if (!length || length > 16 * 1024 * 1024 || offset > INT64_MAX - length)
            throw std::runtime_error("invalid probe range");
        if (nod_disc_open(argv[1], nullptr, &disc) != NOD_RESULT_OK)
            throw std::runtime_error("cannot open source ISO");
        NodDiscHeader header{};
        if (nod_disc_header(disc, &header) != NOD_RESULT_OK ||
            std::memcmp(header.game_id, "RM3E01", 6) || header.disc_num || header.disc_version)
            throw std::runtime_error("unexpected disc revision");
        NodPartitionOptions options{};
        options.validate_hashes = true;
        if (nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, &options, &partition) != NOD_RESULT_OK)
            throw std::runtime_error("cannot open data partition");
        if (nod_seek(partition, static_cast<int64_t>(offset), 0) != static_cast<int64_t>(offset))
            throw std::runtime_error("partition seek failed");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        size_t done = 0;
        while (done < bytes.size()) {
            const int64_t count = nod_read(partition, bytes.data() + done, bytes.size() - done);
            if (count <= 0) throw std::runtime_error("partition read failed or ended early");
            done += static_cast<size_t>(count);
        }
        const std::filesystem::path output(argv[4]);
        std::filesystem::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) throw std::runtime_error("cannot write private probe output");
        uint64_t hash = UINT64_C(14695981039346656037);
        for (uint8_t byte : bytes) hash = (hash ^ byte) * UINT64_C(1099511628211);
        std::cout << "partition_offset=0x" << std::hex << offset << " length=0x" << length
                  << " fnv1a64=0x" << hash << std::dec << " hashes_validated=true\n";
        result = 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << ": " << (nod_error_message() ? nod_error_message() : "no nod error") << '\n';
    }
    nod_free(partition);
    nod_free(disc);
    return result;
}
