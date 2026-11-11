#pragma once
// Retention policy of the persistent Dawn blob cache (dawn_cache.db), kept free of
// GPU and global state so it can be unit tested against a scratch SQLite file
// (tests/blob_cache_policy_test.cpp). gpu_cache.cpp owns the database handle and
// the locking; these functions only operate on the handle they are given.
//
// Table layout (schema 4): cache(key BLOB PRIMARY KEY, value BLOB, size INTEGER,
// compressed INTEGER, last_used INTEGER) plus cache_meta(key TEXT, value TEXT).
// A key is the raw bytes of an XXH128_hash_t (low64, high64), 16 bytes.
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace aurora::webgpu::blob_policy {

using LogFn = std::function<void(const std::string&)>;
using Key = std::pair<uint64_t, uint64_t>;  // (low64, high64)

constexpr size_t KeyBytes = 16;

inline bool exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

inline int64_t pragma_int(sqlite3* db, const char* name) {
  const std::string sql = std::string("PRAGMA ") + name + ";";
  sqlite3_stmt* stmt = nullptr;
  int64_t value = 0;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      value = sqlite3_column_int64(stmt, 0);
    }
  }
  sqlite3_finalize(stmt);
  return value;
}

// Bytes occupied by live rows: the file minus its free pages.
inline int64_t live_bytes(sqlite3* db) {
  return (pragma_int(db, "page_count") - pragma_int(db, "freelist_count")) * pragma_int(db, "page_size");
}

inline int64_t free_bytes(sqlite3* db) { return pragma_int(db, "freelist_count") * pragma_int(db, "page_size"); }

// Freed pages stay inside the file until a VACUUM. Compacts when at least `minFreeBytes`
// and a quarter of the file are free. Returns whether it ran.
inline bool compact_if_wasteful(sqlite3* db, const LogFn& log, int64_t minFreeBytes = 16ll << 20) {
  const int64_t pages = pragma_int(db, "page_count");
  const int64_t freePages = pragma_int(db, "freelist_count");
  const int64_t pageSize = pragma_int(db, "page_size");
  if (freePages * pageSize < minFreeBytes || freePages * 4 < pages) {
    return false;
  }
  if (log) {
    log("compacting: " + std::to_string(freePages * pageSize >> 20) + " of " + std::to_string(pages * pageSize >> 20) +
        " MiB were free pages");
  }
  return exec(db, "VACUUM;");
}

struct CapResult {
  bool over = false;
  size_t evicted = 0;
  int64_t freedBytes = 0;
};

// When the live size exceeds `capBytes`, evicts rows by ascending last_used (oldest first, ties by
// insertion order) until the estimate is at or below 3/4 of the cap. The caller compacts afterwards.
inline CapResult enforce_size_cap(sqlite3* db, int64_t capBytes, const LogFn& log) {
  CapResult result;
  const int64_t live = live_bytes(db);
  if (live <= capBytes) {
    return result;
  }
  result.over = true;
  const int64_t target = capBytes / 4 * 3;
  std::vector<std::string> keys;
  sqlite3_stmt* select = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT key, length(value) FROM cache ORDER BY last_used ASC, rowid ASC", -1, &select,
                         nullptr) != SQLITE_OK) {
    return result;
  }
  while (live - result.freedBytes > target && sqlite3_step(select) == SQLITE_ROW) {
    const auto* blob = static_cast<const char*>(sqlite3_column_blob(select, 0));
    keys.emplace_back(blob, static_cast<size_t>(sqlite3_column_bytes(select, 0)));
    result.freedBytes += sqlite3_column_int64(select, 1);
  }
  sqlite3_finalize(select);
  if (!exec(db, "BEGIN IMMEDIATE")) {
    return result;
  }
  sqlite3_stmt* del = nullptr;
  if (sqlite3_prepare_v2(db, "DELETE FROM cache WHERE key = ?", -1, &del, nullptr) != SQLITE_OK) {
    exec(db, "ROLLBACK");
    return result;
  }
  for (const auto& key : keys) {
    sqlite3_bind_blob(del, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_reset(del);
  }
  sqlite3_finalize(del);
  exec(db, "COMMIT");
  result.evicted = keys.size();
  if (log) {
    log("over the cap: evicted " + std::to_string(result.evicted) + " least recently used blobs");
  }
  return result;
}

inline void ensure_meta(sqlite3* db) {
  exec(db, "CREATE TABLE IF NOT EXISTS cache_meta(key TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL);");
}

inline std::string load_generation(sqlite3* db) {
  ensure_meta(db);
  std::string out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT value FROM cache_meta WHERE key = 'generation'", -1, &stmt, nullptr) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW) {
    if (const auto* text = sqlite3_column_text(stmt, 0)) {
      out = reinterpret_cast<const char*>(text);
    }
  }
  sqlite3_finalize(stmt);
  return out;
}

// Generation labels are application versions: keep letters, digits and . - _ only.
inline std::string sanitize_generation(const char* raw) {
  std::string out;
  if (raw == nullptr) {
    return out;
  }
  for (const char* c = raw; *c != '\0'; ++c) {
    const bool ok = (*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '.' ||
                    *c == '-' || *c == '_';
    out.push_back(ok ? *c : '_');
  }
  return out;
}

inline bool store_generation(sqlite3* db, const std::string& generation) {
  ensure_meta(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO cache_meta(key, value) VALUES('generation', ?)", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, generation.c_str(), static_cast<int>(generation.size()), SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

struct SweepResult {
  bool skipped = false;       // too few touched keys: the launch did not exercise the cache
  size_t removed = 0;
  int64_t removedBytes = 0;
  size_t kept = 0;
};

// Deletes every row whose key is not in `touched` (loaded or stored during this launch), then records
// `generation`. `touched` is sorted and de-duplicated in place. With fewer than `minTouched` keys nothing
// is deleted: a failed or empty prewarm must never wipe valid blobs.
inline SweepResult sweep_untouched(sqlite3* db, std::vector<Key>& touched, const std::string& generation,
                                   size_t minTouched, const LogFn& log) {
  SweepResult result;
  std::sort(touched.begin(), touched.end());
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
  if (touched.size() < minTouched) {
    result.skipped = true;
    if (log) {
      log("sweep skipped: only " + std::to_string(touched.size()) + " blobs were used by this launch");
    }
    return result;
  }
  std::vector<std::string> orphans;
  sqlite3_stmt* select = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT key, length(value) FROM cache", -1, &select, nullptr) != SQLITE_OK) {
    result.skipped = true;
    return result;
  }
  while (sqlite3_step(select) == SQLITE_ROW) {
    const auto* blob = static_cast<const char*>(sqlite3_column_blob(select, 0));
    if (blob == nullptr || sqlite3_column_bytes(select, 0) != static_cast<int>(KeyBytes)) {
      continue;  // not one of ours; never delete what we cannot identify
    }
    Key key;
    std::memcpy(&key.first, blob, 8);
    std::memcpy(&key.second, blob + 8, 8);
    if (std::binary_search(touched.begin(), touched.end(), key)) {
      ++result.kept;
    } else {
      orphans.emplace_back(blob, KeyBytes);
      result.removedBytes += sqlite3_column_int64(select, 1);
    }
  }
  sqlite3_finalize(select);
  if (!exec(db, "BEGIN IMMEDIATE")) {
    result.skipped = true;
    return result;
  }
  sqlite3_stmt* del = nullptr;
  if (sqlite3_prepare_v2(db, "DELETE FROM cache WHERE key = ?", -1, &del, nullptr) != SQLITE_OK) {
    exec(db, "ROLLBACK");
    result.skipped = true;
    return result;
  }
  for (const auto& key : orphans) {
    sqlite3_bind_blob(del, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_reset(del);
  }
  sqlite3_finalize(del);
  store_generation(db, generation);
  exec(db, "COMMIT");
  result.removed = orphans.size();
  return result;
}

}  // namespace aurora::webgpu::blob_policy
