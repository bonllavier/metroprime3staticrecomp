// Read-only: lists a disc's FST, dumps main.dol, or copies one FST file.
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "nod.h"
struct Walk { std::vector<std::pair<uint32_t,std::string>> stack; FILE* out; };
static uint32_t cb(uint32_t i, NodNodeKind k, const char* n, uint32_t size, void* u) {
    auto* w = static_cast<Walk*>(u);
    while (!w->stack.empty() && i >= w->stack.back().first) w->stack.pop_back();
    std::string path;
    for (auto& d : w->stack) path += d.second + "/";
    path += n;
    if (k == NOD_NODE_KIND_DIRECTORY) { if (i) w->stack.push_back({size, std::string(n)}); fprintf(w->out, "D %10u %s\n", size, path.c_str()); }
    else fprintf(w->out, "F %10u %s\n", size, path.c_str());
    return i + 1;
}
static int die(const char* m) { fprintf(stderr, "%s: %s\n", m, nod_error_message() ? nod_error_message() : "?"); return 1; }
int main(int c, char** v) {
    if (c < 3) return 2;
    NodHandle* disc = nullptr; if (nod_disc_open(v[2], nullptr, &disc) != NOD_RESULT_OK) return die("open");
    NodHandle* part = nullptr; NodPartitionOptions o{}; o.validate_hashes = false;
    if (nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, &o, &part) != NOD_RESULT_OK) return die("part");
    std::string mode = v[1];
    if (mode == "list") { Walk w{{}, stdout}; nod_partition_iterate_fst(part, cb, &w); }
    else if (mode == "sys") {
        NodPartitionMeta m{}; if (nod_partition_meta(part, &m) != NOD_RESULT_OK) return die("meta");
        std::string base = v[3];
        FILE* f = fopen((base + "/main.dol").c_str(), "wb"); fwrite(m.raw_dol.data, 1, m.raw_dol.size, f); fclose(f);
        f = fopen((base + "/fst.bin").c_str(), "wb"); fwrite(m.raw_fst.data, 1, m.raw_fst.size, f); fclose(f);
        printf("dol=%llu fst=%llu\n", (unsigned long long)m.raw_dol.size, (unsigned long long)m.raw_fst.size);
    } else if (mode == "file") {
        NodNodeKind k; uint32_t size = 0; uint32_t idx = nod_partition_find_file(part, v[3], &k, &size);
        if (idx == NOD_FST_STOP) return die("find");
        NodHandle* h = nullptr; if (nod_partition_open_file(part, idx, &h) != NOD_RESULT_OK) return die("openfile");
        FILE* f = fopen(v[4], "wb"); std::vector<uint8_t> b(1 << 20); uint64_t tot = 0; int64_t n;
        while ((n = nod_read(h, b.data(), b.size())) > 0) { fwrite(b.data(), 1, (size_t)n, f); tot += n; }
        fclose(f); printf("%s %llu\n", v[3], (unsigned long long)tot); nod_free(h);
    }
    nod_free(part); nod_free(disc); return 0;
}
