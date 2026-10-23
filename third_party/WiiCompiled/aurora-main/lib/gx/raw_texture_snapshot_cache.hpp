#pragma once
#include "source_bytes_equal.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <utility>
#include <vector>

namespace aurora::gx {
// Raw sources have no object-generation contract. Compare every byte before
// reuse; replacing/evicting a cache entry never invalidates submitted handles.
template<class Key, class Handle> class RawTextureSnapshotCache {
  struct Entry {Key key; std::vector<uint8_t> bytes,palette; Handle handle;};
  std::list<Entry> entries;
  size_t bytes = 0;
  size_t budget;
  size_t limit;
  bool retainVersions;
public:
  struct Statistics {uint64_t lookups=0,hits=0,misses=0,evictions=0,replacements=0;};
private:
  Statistics statistics;
public:
  RawTextureSnapshotCache(size_t byteBudget, size_t entryLimit, bool multipleVersions=false)
      : budget(byteBudget), limit(entryLimit), retainVersions(multipleVersions) {}
  Handle Find(const Key& key, const void* data, size_t size, const void* palette=nullptr, size_t paletteSize=0) {
    ++statistics.lookups;
    if(!data || !size || (paletteSize && !palette)) {++statistics.misses;return {};}
    // Most recently used entries live at the back after splice. Search
    // that end first; source-byte validation and LRU eviction stay exact.
    for(auto reverse=entries.rbegin();reverse!=entries.rend();++reverse) {
      auto it=std::prev(reverse.base());
      if(it->key==key && it->bytes.size()==size && it->palette.size()==paletteSize &&
         source_bytes_equal(it->bytes.data(),data,size) &&
         (!paletteSize || source_bytes_equal(it->palette.data(),palette,paletteSize))) {
        auto result=it->handle;
        entries.splice(entries.end(),entries,it);
        ++statistics.hits;
        return result;
      }
    }
    ++statistics.misses;return {};
  }
  void Store(const Key& key, const void* data, size_t size, const Handle& handle,
             const void* palette=nullptr, size_t paletteSize=0) {
    if(!data || !size || size>budget || paletteSize>budget-size || !limit || (paletteSize && !palette)) return;
    const size_t total=size+paletteSize;
    for(auto it=entries.begin();it!=entries.end();) {
      const bool sameBytes=retainVersions && it->key==key && it->bytes.size()==size && it->palette.size()==paletteSize &&
          source_bytes_equal(it->bytes.data(),data,size) && (!paletteSize || source_bytes_equal(it->palette.data(),palette,paletteSize));
      if(it->key==key && (!retainVersions || sameBytes)) {
        bytes-=it->bytes.size()+it->palette.size();it=entries.erase(it);++statistics.replacements;
      } else ++it;
    }
    while(!entries.empty() && (entries.size()>=limit || total>budget-bytes)) {
      bytes-=entries.front().bytes.size()+entries.front().palette.size();entries.pop_front();
      ++statistics.evictions;
    }
    const auto* begin=static_cast<const uint8_t*>(data);
    std::vector<uint8_t> paletteBytes;
    if(paletteSize) {
      const auto* paletteBegin=static_cast<const uint8_t*>(palette);
      paletteBytes.assign(paletteBegin,paletteBegin+paletteSize);
    }
    entries.push_back({key,std::vector<uint8_t>(begin,begin+size),std::move(paletteBytes),handle});
    bytes+=total;
  }
  size_t ByteSize() const {return bytes;}
  size_t EntryCount() const {return entries.size();}
  Statistics Stats() const {return statistics;}
};
}
