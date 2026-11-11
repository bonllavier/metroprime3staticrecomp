#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <limits>
#include <string>
#include <filesystem>
#include <vector>

#include "../fs_helper.hpp"
#include "../internal.hpp"
#include "../sqlite_utils.hpp"
#include "blob_cache_policy.hpp"
#include "gpu.hpp"

#include <sqlite3.h>
#include <fmt/format.h>
#if defined(AURORA_CACHE_USE_ZSTD)
#include <zstd.h>
#endif
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace aurora::webgpu {
static Module Log("aurora::gpu::cache");

static sqlite3* db;
static sqlite3_stmt* load_stmt;
static sqlite3_stmt* store_stmt;
static sqlite3_stmt* touch_stmt;
static bool cache_broken;
static std::mutex cache_mutex;
#if defined(AURORA_CACHE_USE_ZSTD)
static std::vector<uint8_t> compress_buffer;
#endif

// Schema 3 added last_used (whole days since the Unix epoch) so stale blobs can be
// pruned: config-version bumps and driver updates change every Dawn cache key, and
// without an age column the orphaned rows accumulate forever (observed 1.6 GB).
// Schema 4 is a content reset for the vulkan_monolithic_pipeline_cache switch, which
// obsoletes every per-pipeline blob at once.
constexpr int CACHE_SCHEMA = 4;
// Seven days: a build update re-keys what the generator changed, and anything the
// current build stopped asking for is dead weight. The launch sweep below removes it
// at once on a version change; this age limit covers driver updates and API switches.
constexpr int64_t PruneAfterDays = 7;
// Hard ceiling on the live size of dawn_cache.db (WII_COMPILED_AURORA_DAWN_CACHE_MB,
// 64..16384). Over it, the least recently used blobs go first, down to 3/4 of the cap.
constexpr uint64_t DefaultSizeCapMiB = 400;
// Rows whose last_used lags today are collected in memory and written in one batch at
// shutdown: the load path sits inside a read transaction that is always rolled back,
// and day granularity makes anything more eager pointless.

static std::atomic<uint64_t> g_lookups{0};
static std::atomic<uint64_t> g_hits{0};
static std::atomic<uint64_t> g_stores{0};
static std::atomic<uint64_t> g_hitBytes{0};
static std::vector<XXH128_hash_t> g_pendingTouches;
// Keys loaded or stored during this launch, for the generation sweep.
static std::vector<blob_policy::Key> g_touchedKeys;
static std::string g_storedGeneration;
static bool g_generationChanged = false;
static bool g_compactAtShutdown = false;
// A launch that touched fewer blobs than this did not exercise the cache; it never sweeps.
constexpr size_t MinTouchedKeysToSweep = 64;

static std::string current_generation() { return blob_policy::sanitize_generation(g_config.cacheGeneration); }

static int64_t days_now() { return static_cast<int64_t>(std::time(nullptr) / 86400); }

static void init_abort() {
  cache_broken = true;
  sqlite3_close(db);
  db = nullptr;
}

static int check(int ret) {
  if (ret != SQLITE_OK) {
    Log.error("SQLite operation failed: {}", sqlite3_errmsg(db));
  }

  return ret;
}

enum class SchemaState { Match, Mismatch, Error };

static SchemaState check_schema() {
  auto ret = sqlite::exec(db, "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);");
  if (ret != SQLITE_OK) {
    Log.error("Failed to create schema table: {}", sqlite3_errmsg(db));
    return SchemaState::Error;
  }

  bool match = false;
  const auto cmd = fmt::format("SELECT * FROM aurora_schema WHERE value = {}", CACHE_SCHEMA);
  ret = sqlite::exec(db, cmd.c_str(), [&match](int, char**, char**) { match = true; }, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to check schema table: {}", sqlite3_errmsg(db));
    return SchemaState::Error;
  }
  return match ? SchemaState::Match : SchemaState::Mismatch;
}

static bool create_schema() {
  sqlite::Transaction tx(db, Log, true);
  if (!tx) {
    Log.error("Failed to open schema transaction: {}", sqlite3_errmsg(db));
    return false;
  }
  const auto cmd = fmt::format(
      R"(CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);
DROP TABLE IF EXISTS cache;
CREATE TABLE cache (
  key BLOB PRIMARY KEY NOT NULL,
  value BLOB NOT NULL,
  size INTEGER NOT NULL,
  compressed INTEGER NOT NULL,
  last_used INTEGER NOT NULL DEFAULT 0
);
DELETE FROM aurora_schema;
INSERT INTO aurora_schema VALUES ({});)",
      CACHE_SCHEMA);
  const auto ret = sqlite::exec(db, cmd.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to create schema: {}", sqlite3_errmsg(db));
    return false;
  }
  tx.commit();
  return true;
}

static void prune_stale_rows() {
  const auto cmd = fmt::format("DELETE FROM cache WHERE last_used < {}", days_now() - PruneAfterDays);
  auto ret = sqlite::exec(db, cmd.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune stale cache rows: {}", sqlite3_errmsg(db));
    return;
  }
  const auto pruned = sqlite3_changes(db);
  if (pruned > 0) {
    Log.info("Pruned {} stale Dawn cache blobs (unused for {}+ days)", pruned, PruneAfterDays);
  }
  // Freed pages are only reused, never returned to the filesystem, so compact when a
  // meaningful amount was dropped. Blobs run hundreds of KB each, making even a few
  // hundred rows a noticeable slice of the file.
  if (pruned > 256) {
    ret = sqlite::exec(db, "VACUUM;");
    if (ret != SQLITE_OK) {
      Log.warn("Failed to vacuum Dawn cache after pruning: {}", sqlite3_errmsg(db));
    }
  }
}

static void log_policy(const std::string& message) { Log.info("{}", message); }

static uint64_t size_cap_mib() {
  uint64_t capMiB = DefaultSizeCapMiB;
  if (const char* value = std::getenv("WII_COMPILED_AURORA_DAWN_CACHE_MB")) {
    const auto parsed = std::strtoull(value, nullptr, 10);
    if (parsed >= 64 && parsed <= 16384) capMiB = parsed;
  }
  return capMiB;
}

static void load_generation_meta() {
  g_storedGeneration = blob_policy::load_generation(db);
  const auto current = current_generation();
  g_generationChanged = !current.empty() && current != g_storedGeneration;
  if (g_generationChanged) {
    Log.info("Shader cache generation changed ('{}' -> '{}'): obsolete blobs are removed after the boot prewarm",
             g_storedGeneration.empty() ? "none" : g_storedGeneration, current);
  }
}

static bool cache_init_core() {
  Log.debug("SQLite version {}", sqlite3_libversion());

  const auto path = fs_path_from_string(g_config.cachePath) / "dawn_cache.db";
  std::string file = fs_path_to_string(path);
  Log.debug("Using dawn cache at {}", file);
  auto ret = sqlite3_open(file.c_str(), &db);
  if (ret != SQLITE_OK) {
    Log.error("Failed to open database: {}", sqlite3_errmsg(db));
    return false;
  }

  // WAL mode + NORMAL = no need for disk syncs, consistent but not durable is fine.
  ret = sqlite::exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
  if (ret != SQLITE_OK) {
    Log.error("Failed to set pragmas: {}", sqlite3_errmsg(db));
    return false;
  }

  switch (check_schema()) {
  case SchemaState::Match:
    prune_stale_rows();
    blob_policy::enforce_size_cap(db, static_cast<int64_t>(size_cap_mib() << 20), log_policy);
    blob_policy::compact_if_wasteful(db, log_policy);
    break;
  case SchemaState::Mismatch: {
    // Dropping the table would leave the freed pages inside the file, so a schema
    // change deletes the database outright; pre-schema-3 files had grown unbounded.
    Log.info("Dawn cache schema changed; recreating '{}'", file);
    sqlite3_close(db);
    db = nullptr;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto wal = path;
    wal += "-wal";
    std::filesystem::remove(wal, ec);
    auto shm = path;
    shm += "-shm";
    std::filesystem::remove(shm, ec);
    ret = sqlite3_open(file.c_str(), &db);
    if (ret != SQLITE_OK) {
      Log.error("Failed to recreate database: {}", sqlite3_errmsg(db));
      return false;
    }
    ret = sqlite::exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    if (ret != SQLITE_OK) {
      Log.error("Failed to set pragmas: {}", sqlite3_errmsg(db));
      return false;
    }
    if (!create_schema()) {
      return false;
    }
    break;
  }
  case SchemaState::Error:
    return false;
  }
  load_generation_meta();

  ret = sqlite3_prepare_v3(db, "SELECT value, size, compressed, last_used FROM cache WHERE key = ?", -1,
                           SQLITE_PREPARE_PERSISTENT, &load_stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return false;
  }

  ret = sqlite3_prepare_v3(db, "REPLACE INTO cache (key, value, size, compressed, last_used) VALUES (?, ?, ?, ?, ?)",
                           -1, SQLITE_PREPARE_PERSISTENT, &store_stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return false;
  }

  ret = sqlite3_prepare_v3(db, "UPDATE cache SET last_used = ? WHERE key = ?", -1, SQLITE_PREPARE_PERSISTENT,
                           &touch_stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return false;
  }

  return true;
}

// Caller holds cache_mutex. Writes the queued last_used refreshes in one transaction.
static void flush_touches() {
  if (g_pendingTouches.empty() || db == nullptr || touch_stmt == nullptr) {
    return;
  }

  sqlite::Transaction tx(db, Log, true);
  if (!tx) {
    Log.error("Failed to open touch transaction");
    g_pendingTouches.clear();
    return;
  }
  const auto today = days_now();
  for (const auto& keyHash : g_pendingTouches) {
    check(sqlite3_bind_int64(touch_stmt, 1, today));
    check(sqlite3_bind_blob(touch_stmt, 2, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT));
    if (sqlite3_step(touch_stmt) != SQLITE_DONE) {
      Log.error("Failed to refresh cache row age: {}", sqlite3_errmsg(db));
    }
    check(sqlite3_reset(touch_stmt));
  }
  g_pendingTouches.clear();
  tx.commit();
}

static bool cache_init() {
  if (cache_broken) {
    return false;
  }

  if (db) {
    return true;
  }

  if (!cache_init_core()) {
    Log.error("SQLite DB init failed");
    init_abort();
    return false;
  }

  Log.debug("SQLite cache init succeeded");

  return true;
}

// Test switch WII_COMPILED_DAWN_CACHE_COLD_AFTER_BOOT=1: once the boot prewarm has drained, every lookup
// misses, so shaders met in play compile cold (as for a player whose cache lacks them).
static std::atomic<bool> g_bootPrewarmDrained{false};
static bool cold_after_boot() {
  static const bool enabled = std::getenv("WII_COMPILED_DAWN_CACHE_COLD_AFTER_BOOT") != nullptr;
  return enabled;
}

size_t load_from_cache(void const* key, size_t keySize, void* value, size_t valueSize, void*) {
  if (cold_after_boot() && g_bootPrewarmDrained.load(std::memory_order_relaxed)) {
    return 0;
  }
  std::lock_guard lock(cache_mutex);

  if (!cache_init()) {
    return 0;
  }

  sqlite::Transaction tx(db, Log);
  if (!tx) {
    Log.error("Failed to open load transaction");
    return 0;
  }

  // Dawn probes with value == nullptr for the size first, then fetches; count each
  // probe as one logical lookup so the hit rate reads per-entry.
  if (value == nullptr) {
    g_lookups.fetch_add(1, std::memory_order_relaxed);
  }

  const auto keyHash = XXH128(key, keySize, 0);
  check(sqlite3_bind_blob(load_stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT));

  const auto ret = sqlite3_step(load_stmt);
  size_t foundSize;
  if (ret == SQLITE_ROW) {
    // Hit
    const auto foundPtr = sqlite3_column_blob(load_stmt, 0);
    const auto declaredSize = sqlite3_column_int64(load_stmt, 1);
    const auto storedSize = sqlite3_column_bytes(load_stmt, 0);
    const auto compression = sqlite3_column_int(load_stmt, 2);
    const bool compressed = compression == 1;
    // Dawn asks for the size before allocating its destination. Validate here,
    // not only during the copy: corrupt metadata must become a cache miss.
    bool valid = declaredSize > 0 &&
                 static_cast<uint64_t>(declaredSize) <= std::numeric_limits<size_t>::max() &&
                 foundPtr != nullptr && storedSize > 0 && (compression == 0 || compression == 1);
    if (valid && compressed) {
#if defined(AURORA_CACHE_USE_ZSTD)
      // Our writer uses ZSTD_compress, which records the original content size.
      const auto frameSize = ZSTD_getFrameContentSize(foundPtr, static_cast<size_t>(storedSize));
      valid = frameSize != ZSTD_CONTENTSIZE_ERROR && frameSize != ZSTD_CONTENTSIZE_UNKNOWN &&
              frameSize == static_cast<uint64_t>(declaredSize);
#else
      valid = false;
#endif
    } else if (valid) {
      valid = declaredSize == storedSize;
    }
    if (!valid) {
      Log.error("Ignoring cache entry with inconsistent size or compression metadata");
      check(sqlite3_reset(load_stmt));
      return 0;
    }
    foundSize = static_cast<size_t>(declaredSize);
    if (value == nullptr) {
      g_hits.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_hitBytes.fetch_add(static_cast<uint64_t>(foundSize), std::memory_order_relaxed);
    }
    if (sqlite3_column_int64(load_stmt, 3) != days_now()) {
      g_pendingTouches.push_back(keyHash);
    }
    if (g_generationChanged) {
      g_touchedKeys.emplace_back(keyHash.low64, keyHash.high64);
    }

    if (value && valueSize == foundSize) {
      if (compressed) {
#if defined(AURORA_CACHE_USE_ZSTD)
        const auto compSize = sqlite3_column_bytes(load_stmt, 0);
        const auto zstdRet = ZSTD_decompress(value, valueSize, foundPtr, compSize);
        if (ZSTD_isError(zstdRet)) {
          Log.error("zstd decompression error: {}", ZSTD_getErrorName(zstdRet));
          foundSize = 0;
        } else if (zstdRet != foundSize) {
          Log.error("zstd decompression size mismatch: expected {}, got {}", foundSize, zstdRet);
          foundSize = 0;
        }
#else
        Log.error("Cache entry is zstd-compressed but zstd support is disabled");
        foundSize = 0;
#endif
      } else {
        if (foundSize != 0 && !foundPtr) {
          Log.error("Cache entry is missing raw value data");
          foundSize = 0;
        } else if (foundSize != 0) {
          std::memcpy(value, foundPtr, foundSize);
        }
      }
    }
  } else if (ret == SQLITE_DONE) {
    // Miss
    foundSize = 0;
  } else {
    Log.error("Looking up cache key failed: {}", sqlite3_errmsg(db));
    return 0;
  }

  check(sqlite3_reset(load_stmt));

  return foundSize;
}

void store_to_cache(void const* key, size_t keySize, void const* value, size_t valueSize, void*) {
  std::lock_guard lock(cache_mutex);

  if (!cache_init()) {
    return;
  }

  sqlite::Transaction tx(db, Log, true);
  if (!tx) {
    Log.error("Failed to open store transaction");
    return;
  }

  const void* storedValue = value;
  sqlite3_uint64 storedValueSize = valueSize;
  int compressed = 0;
#if defined(AURORA_CACHE_USE_ZSTD)
  const auto bound = ZSTD_compressBound(valueSize);
  if (ZSTD_isError(bound)) {
    Log.error("Failed to calculate ZSTD_compressBound: {}", ZSTD_getErrorName(bound));
    return;
  }

  if (compress_buffer.size() < bound) {
    compress_buffer.resize(bound);
  }

  const auto compressRet = ZSTD_compress(compress_buffer.data(), compress_buffer.size(), value, valueSize, 0);
  if (ZSTD_isError(compressRet)) {
    Log.error("ZSTD compression error: {}", ZSTD_getErrorName(compressRet));
    return;
  }

  if (compressRet < valueSize) {
    storedValue = compress_buffer.data();
    storedValueSize = compressRet;
    compressed = 1;
  }
#endif

  const auto keyHash = XXH128(key, keySize, 0);
  check(sqlite3_bind_blob64(store_stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT));
  check(
      sqlite3_bind_blob64(store_stmt, 2, storedValue, storedValueSize, compressed ? SQLITE_STATIC : SQLITE_TRANSIENT));
  check(sqlite3_bind_int64(store_stmt, 3, static_cast<sqlite3_int64>(valueSize)));
  check(sqlite3_bind_int(store_stmt, 4, compressed));
  check(sqlite3_bind_int64(store_stmt, 5, days_now()));
  g_stores.fetch_add(1, std::memory_order_relaxed);
  if (g_generationChanged) {
    g_touchedKeys.emplace_back(keyHash.low64, keyHash.high64);
  }

  const auto ret = sqlite3_step(store_stmt);
  if (ret != SQLITE_DONE) {
    // Error or something
    Log.error("Failed to insert row: {}", sqlite3_errmsg(db));
    return;
  }

  check(sqlite3_reset(store_stmt));
  check(sqlite3_bind_null(store_stmt, 2));
  check(sqlite3_bind_null(store_stmt, 4));

  tx.commit();
}

void blob_cache_finish_boot_sweep() noexcept {
  g_bootPrewarmDrained.store(true, std::memory_order_relaxed);
  std::lock_guard lock(cache_mutex);
  if (db == nullptr || cache_broken || !g_generationChanged) {
    return;
  }
  g_generationChanged = false;
  const auto current = current_generation();
  const auto result = blob_policy::sweep_untouched(db, g_touchedKeys, current, MinTouchedKeysToSweep, log_policy);
  g_touchedKeys.clear();
  g_touchedKeys.shrink_to_fit();
  if (result.skipped) {
    return;  // retried on the next launch: the stored generation was not updated
  }
  Log.info("Shader cache cleanup for build '{}' (previous '{}'): removed {} blobs not used by this launch ({:.1f} MiB), kept {}",
           current, g_storedGeneration.empty() ? "none" : g_storedGeneration, result.removed,
           static_cast<double>(result.removedBytes) / 1048576.0, result.kept);
  g_storedGeneration = current;
  g_compactAtShutdown = result.removed > 0;
}

void cache_shutdown() {
  std::lock_guard lock(cache_mutex);
#if defined(AURORA_CACHE_USE_ZSTD)
  compress_buffer.clear();
#endif
  flush_touches();
  check(sqlite3_finalize(load_stmt));
  check(sqlite3_finalize(store_stmt));
  check(sqlite3_finalize(touch_stmt));
  load_stmt = nullptr;
  store_stmt = nullptr;
  touch_stmt = nullptr;
  if (g_compactAtShutdown && db != nullptr) {
    // After the sweep: return the freed pages to the filesystem while nothing else uses the cache.
    g_compactAtShutdown = false;
    if (sqlite::exec(db, "VACUUM;") != SQLITE_OK) {
      Log.warn("Failed to compact the Dawn cache at shutdown: {}", sqlite3_errmsg(db));
    }
  }
  check(sqlite3_close(db));
  db = nullptr;
}

BlobCacheStats blob_cache_stats() noexcept {
  return BlobCacheStats{
      .lookups = g_lookups.load(std::memory_order_relaxed),
      .hits = g_hits.load(std::memory_order_relaxed),
      .stores = g_stores.load(std::memory_order_relaxed),
      .hitBytes = g_hitBytes.load(std::memory_order_relaxed),
  };
}

} // namespace aurora::webgpu
