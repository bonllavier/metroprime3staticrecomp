// Unit tests of the Dawn blob cache retention policy (lib/webgpu/blob_cache_policy.hpp).
// They run against a scratch SQLite file: no GPU, no game, no Aurora globals.
#include <gtest/gtest.h>

#include "../lib/webgpu/blob_cache_policy.hpp"

#include <cstdio>
#include <filesystem>
#include <random>

namespace bp = aurora::webgpu::blob_policy;

namespace {

class BlobCachePolicyTest : public ::testing::Test {
protected:
  void SetUp() override {
    path = std::filesystem::temp_directory_path() /
           ("aurora_blob_cache_policy_" + std::to_string(std::random_device{}()) + ".db");
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &db), SQLITE_OK);
    ASSERT_TRUE(bp::exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"));
    ASSERT_TRUE(bp::exec(db,
                         "CREATE TABLE cache (key BLOB PRIMARY KEY NOT NULL, value BLOB NOT NULL, size INTEGER NOT NULL, "
                         "compressed INTEGER NOT NULL, last_used INTEGER NOT NULL DEFAULT 0);"));
  }
  void TearDown() override {
    sqlite3_close(db);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    for (const char* suffix : {"-wal", "-shm"}) {
      std::filesystem::remove(path.string() + suffix, ec);
    }
  }

  static bp::Key make_key(uint64_t n) { return {n, n * 0x9E3779B97F4A7C15ull}; }

  void insert(uint64_t n, size_t bytes, int64_t lastUsed) {
    const auto key = make_key(n);
    unsigned char raw[16];
    std::memcpy(raw, &key.first, 8);
    std::memcpy(raw + 8, &key.second, 8);
    std::vector<unsigned char> value(bytes);
    std::mt19937 rng(static_cast<uint32_t>(n));
    for (auto& b : value) b = static_cast<unsigned char>(rng());  // incompressible, so sizes are predictable
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "INSERT INTO cache(key,value,size,compressed,last_used) VALUES(?,?,?,0,?)", -1, &stmt,
                                 nullptr), SQLITE_OK);
    sqlite3_bind_blob(stmt, 1, raw, 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(bytes));
    sqlite3_bind_int64(stmt, 4, lastUsed);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
  }

  int64_t count() {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT count(*) FROM cache", -1, &stmt, nullptr);
    sqlite3_step(stmt);
    const int64_t n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
  }

  bool has(uint64_t n) {
    const auto key = make_key(n);
    unsigned char raw[16];
    std::memcpy(raw, &key.first, 8);
    std::memcpy(raw + 8, &key.second, 8);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT 1 FROM cache WHERE key = ?", -1, &stmt, nullptr);
    sqlite3_bind_blob(stmt, 1, raw, 16, SQLITE_TRANSIENT);
    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
  }

  std::filesystem::path path;
  sqlite3* db = nullptr;
  std::vector<std::string> logLines;
  bp::LogFn log = [this](const std::string& line) { logLines.push_back(line); };
};

TEST_F(BlobCachePolicyTest, SweepKeepsTouchedBlobsAndRemovesTheRest) {
  for (uint64_t n = 1; n <= 200; ++n) insert(n, 2000, 100);
  std::vector<bp::Key> touched;
  for (uint64_t n = 1; n <= 200; n += 2) touched.push_back(make_key(n));  // odd keys are live
  touched.push_back(make_key(1));                                          // duplicates are tolerated
  const auto result = bp::sweep_untouched(db, touched, "0.2.0", 64, log);
  EXPECT_FALSE(result.skipped);
  EXPECT_EQ(result.kept, 100u);
  EXPECT_EQ(result.removed, 100u);
  EXPECT_EQ(count(), 100);
  EXPECT_TRUE(has(1));
  EXPECT_FALSE(has(2));
  EXPECT_GE(result.removedBytes, 100 * 2000);
  EXPECT_EQ(bp::load_generation(db), "0.2.0");
}

TEST_F(BlobCachePolicyTest, SweepDoesNothingWhenTheLaunchBarelyUsedTheCache) {
  for (uint64_t n = 1; n <= 200; ++n) insert(n, 500, 100);
  bp::store_generation(db, "0.1.0");
  std::vector<bp::Key> touched{make_key(1), make_key(2), make_key(3)};  // a failed prewarm
  const auto result = bp::sweep_untouched(db, touched, "0.2.0", 64, log);
  EXPECT_TRUE(result.skipped);
  EXPECT_EQ(count(), 200);                           // nothing deleted
  EXPECT_EQ(bp::load_generation(db), "0.1.0");       // so the next launch retries
}

TEST_F(BlobCachePolicyTest, SweepIgnoresRowsWithAForeignKeyShape) {
  for (uint64_t n = 1; n <= 100; ++n) insert(n, 300, 100);
  const unsigned char shortKey[4] = {1, 2, 3, 4};
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db, "INSERT INTO cache(key,value,size,compressed,last_used) VALUES(?,x'00',1,0,0)", -1, &stmt, nullptr);
  sqlite3_bind_blob(stmt, 1, shortKey, 4, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  std::vector<bp::Key> touched;
  for (uint64_t n = 1; n <= 100; ++n) touched.push_back(make_key(n));
  const auto result = bp::sweep_untouched(db, touched, "g", 64, log);
  EXPECT_EQ(result.removed, 0u);
  EXPECT_EQ(count(), 101);  // the odd row is never ours to delete
}

TEST_F(BlobCachePolicyTest, GenerationRoundTripsAndIsSanitized) {
  EXPECT_EQ(bp::load_generation(db), "");
  EXPECT_TRUE(bp::store_generation(db, "0.1.2"));
  EXPECT_EQ(bp::load_generation(db), "0.1.2");
  EXPECT_TRUE(bp::store_generation(db, "0.1.3"));
  EXPECT_EQ(bp::load_generation(db), "0.1.3");
  EXPECT_EQ(bp::sanitize_generation("0.1.2"), "0.1.2");
  // Every character outside letters, digits and . - _ becomes '_' ('-' is allowed, so "--" survives).
  EXPECT_EQ(bp::sanitize_generation("1.0'; DROP TABLE cache;--"), "1.0___DROP_TABLE_cache_--");
  EXPECT_EQ(bp::sanitize_generation("a b'c\"d"), "a_b_c_d");
  EXPECT_EQ(bp::sanitize_generation(nullptr), "");
}

TEST_F(BlobCachePolicyTest, SizeCapEvictsTheLeastRecentlyUsedFirst) {
  // 40 blobs of 50 KB = 2 MB. Older rows have smaller last_used.
  for (uint64_t n = 1; n <= 40; ++n) insert(n, 50 * 1024, static_cast<int64_t>(n));
  const int64_t before = bp::live_bytes(db);
  ASSERT_GT(before, 1900 * 1024);
  const auto result = bp::enforce_size_cap(db, 1000 * 1024, log);
  EXPECT_TRUE(result.over);
  EXPECT_GT(result.evicted, 0u);
  EXPECT_FALSE(has(1));                // the oldest went first
  EXPECT_TRUE(has(40));                // the newest survived
  // Every survivor is newer than every evicted row.
  int64_t newestEvicted = 0;
  for (uint64_t n = 1; n <= 40; ++n) if (!has(n)) newestEvicted = static_cast<int64_t>(n);
  for (uint64_t n = static_cast<uint64_t>(newestEvicted) + 1; n <= 40; ++n) EXPECT_TRUE(has(n)) << n;
  // The estimate is brought to 3/4 of the cap (not just below it).
  EXPECT_LE(before - result.freedBytes, 1000 * 1024 / 4 * 3 + 60 * 1024);
}

TEST_F(BlobCachePolicyTest, SizeCapLeavesASmallCacheAlone) {
  for (uint64_t n = 1; n <= 10; ++n) insert(n, 1000, 1);
  const auto result = bp::enforce_size_cap(db, 64ll << 20, log);
  EXPECT_FALSE(result.over);
  EXPECT_EQ(count(), 10);
}

TEST_F(BlobCachePolicyTest, CompactionReturnsFreePagesToTheFile) {
  for (uint64_t n = 1; n <= 300; ++n) insert(n, 64 * 1024, 1);  // about 19 MB
  ASSERT_TRUE(bp::exec(db, "DELETE FROM cache WHERE rowid % 10 != 0;"));  // 90 % of it becomes free pages
  const auto sizeBefore = std::filesystem::file_size(path);
  EXPECT_GT(bp::free_bytes(db), 10ll << 20);
  EXPECT_FALSE(bp::compact_if_wasteful(db, log, 64ll << 20));  // threshold above what is free: no-op
  EXPECT_TRUE(bp::compact_if_wasteful(db, log, 8ll << 20));
  bp::exec(db, "PRAGMA wal_checkpoint(TRUNCATE);");
  EXPECT_LT(std::filesystem::file_size(path), sizeBefore / 3);
  EXPECT_EQ(count(), 30);
  EXPECT_LT(bp::free_bytes(db), 1ll << 20);
}

TEST_F(BlobCachePolicyTest, CompactionSkipsAHealthyFile) {
  for (uint64_t n = 1; n <= 100; ++n) insert(n, 64 * 1024, 1);
  EXPECT_FALSE(bp::compact_if_wasteful(db, log, 1));  // nothing is free
}

}  // namespace
