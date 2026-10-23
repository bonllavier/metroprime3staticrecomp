#include "hle/nand_directory_usage.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using NandDirectoryUsage::Error;

static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void File(const fs::path& path, size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << std::string(bytes, 'x');
    stream.close();
    Require(static_cast<bool>(stream), "Fixture write failed");
}

int main(int argc, char** argv) {
    try {
        Require(argc <= 2, "Pass at most one new fixture directory");
        const fs::path root = argc == 2 ? fs::absolute(argv[1]) :
            fs::current_path() / ("nand_usage_fixture_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        Require(!fs::exists(root), "Fixture already exists; refusing to overwrite it");
        fs::create_directories(root);
        auto result = NandDirectoryUsage::Measure(root);
        Require(result.error == Error::None && result.clusters == 0 && result.inodes == 1,
                "Empty directory must include itself");
        File(root / "empty", 0);
        File(root / "one", 1);
        File(root / "exact", 16384);
        File(root / "two", 16385);
        File(root / "nested" / "item", 7);
        File(root / ".host_metadata_not_nand", 100);
        File(root / "host_directory_too_long" / "item", 100);
        result = NandDirectoryUsage::Measure(root);
        Require(result.error == Error::None && result.clusters == 5 && result.inodes == 7,
                "Real tree cluster rounding or recursive inode count differs");
        const auto nested = NandDirectoryUsage::Measure(root / "nested");
        Require(nested.error == Error::None && nested.clusters == 1 && nested.inodes == 2,
                "Subdirectory measurement differs");
        Require(NandDirectoryUsage::Measure(root / "absent").error == Error::Missing,
                "Missing directory must fail");
        Require(NandDirectoryUsage::Measure(root / "one").error == Error::Invalid,
                "A regular file is not an IOS directory query");
        Require(NandDirectoryUsage::Measure({}).error == Error::Invalid,
                "Empty translated path must fail");
        Require(NandDirectoryUsage::ValidVectorSizes(1, 2, 64, 4, 4), "SDK vector layout rejected");
        Require(!NandDirectoryUsage::ValidVectorSizes(2, 1, 64, 4, 4), "Wrong vector direction accepted");
        Require(!NandDirectoryUsage::ValidVectorSizes(1, 2, 63, 4, 4), "Short path vector accepted");
        Require(!NandDirectoryUsage::ValidVectorSizes(1, 2, 64, 8, 4), "Oversized result accepted");
        Require(!NandDirectoryUsage::ValidVectorSizes(1, 2, 64, 4, 3), "Short result accepted");
        std::cout << "PASS filesystem-backed IOS usage: 5 clusters / 7 inodes, errors and vector layout\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
