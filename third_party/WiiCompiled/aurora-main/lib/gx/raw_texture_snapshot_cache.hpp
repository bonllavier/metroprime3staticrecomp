#pragma once
#include "source_bytes_equal.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aurora::gx {
// Raw sources have no object-generation contract. Compare every byte before
// reuse; replacing/evicting a cache entry never invalidates submitted handles.
template<class Key, class Handle> class RawTextureSnapshotCache {
  struct Entry {
    Key key;
    std::vector<uint8_t> bytes,palette;
    Handle handle;
    size_t keyHash=0;
    uint64_t sourceGeneration=0,paletteGeneration=0;
    bool indexed=false,generationsTracked=false;
  };
  std::list<Entry> entries;
  using EntryIterator=typename std::list<Entry>::iterator;
  std::unordered_multimap<size_t,EntryIterator> keyIndex;
  size_t bytes = 0;
  size_t budget;
  size_t limit;
  bool retainVersions;
public:
  struct Statistics {uint64_t lookups=0,hits=0,misses=0,evictions=0,replacements=0,keyProbes=0,byteValidations=0,generationHits=0;};
private:
  Statistics statistics;
  void erase_indexed(EntryIterator entry) {
    if(!entry->indexed)return;
    const auto [first,last]=keyIndex.equal_range(entry->keyHash);
    for(auto index=first;index!=last;++index) {
      if(index->second==entry) {keyIndex.erase(index);return;}
    }
  }
  void erase_entry(EntryIterator entry) {
    erase_indexed(entry);
    bytes-=entry->bytes.size()+entry->palette.size();
    entries.erase(entry);
  }
public:
  RawTextureSnapshotCache(size_t byteBudget, size_t entryLimit, bool multipleVersions=false)
      : budget(byteBudget), limit(entryLimit), retainVersions(multipleVersions) {}
  Handle Find(const Key& key, const void* data, size_t size, const void* palette=nullptr, size_t paletteSize=0,
              size_t keyHash=0, bool useKeyHash=false,uint64_t sourceGeneration=0,
              uint64_t paletteGeneration=0,bool useGenerations=false) {
    ++statistics.lookups;
    if(!data || !size || (paletteSize && !palette)) {++statistics.misses;return {};}
    if(useKeyHash) {
      const auto [first,last]=keyIndex.equal_range(keyHash);
      for(auto index=first;index!=last;++index) {
        ++statistics.keyProbes;
        auto it=index->second;
        if(it->key==key && it->bytes.size()==size && it->palette.size()==paletteSize) {
          const bool generationsMatch=useGenerations && it->generationsTracked &&
              it->sourceGeneration==sourceGeneration && it->paletteGeneration==paletteGeneration;
          const bool bytesMatch=generationsMatch || [&] {
            ++statistics.byteValidations;
            return source_bytes_equal(it->bytes.data(),data,size) &&
                (!paletteSize || source_bytes_equal(it->palette.data(),palette,paletteSize));
          }();
          if(!bytesMatch)continue;
          if(useGenerations) {
            it->sourceGeneration=sourceGeneration;
            it->paletteGeneration=paletteGeneration;
            it->generationsTracked=true;
          }
          auto result=it->handle;
          entries.splice(entries.end(),entries,it);
          ++statistics.hits;
          if(generationsMatch)++statistics.generationHits;
          return result;
        }
      }
      ++statistics.misses;return {};
    }
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
             const void* palette=nullptr, size_t paletteSize=0,size_t keyHash=0,bool useKeyHash=false,
             uint64_t sourceGeneration=0,uint64_t paletteGeneration=0,bool useGenerations=false) {
    if(!data || !size || size>budget || paletteSize>budget-size || !limit || (paletteSize && !palette)) return;
    const size_t total=size+paletteSize;
    for(auto it=entries.begin();it!=entries.end();) {
      const bool sameBytes=retainVersions && it->key==key && it->bytes.size()==size && it->palette.size()==paletteSize &&
          source_bytes_equal(it->bytes.data(),data,size) && (!paletteSize || source_bytes_equal(it->palette.data(),palette,paletteSize));
      if(it->key==key && (!retainVersions || sameBytes)) {
        auto erase=it++;erase_entry(erase);++statistics.replacements;
      } else ++it;
    }
    while(!entries.empty() && (entries.size()>=limit || total>budget-bytes)) {
      erase_entry(entries.begin());
      ++statistics.evictions;
    }
    const auto* begin=static_cast<const uint8_t*>(data);
    std::vector<uint8_t> paletteBytes;
    if(paletteSize) {
      const auto* paletteBegin=static_cast<const uint8_t*>(palette);
      paletteBytes.assign(paletteBegin,paletteBegin+paletteSize);
    }
    entries.push_back({key,std::vector<uint8_t>(begin,begin+size),std::move(paletteBytes),handle,
                       keyHash,sourceGeneration,paletteGeneration,useKeyHash,useGenerations});
    if(useKeyHash)keyIndex.emplace(keyHash,std::prev(entries.end()));
    bytes+=total;
  }
  size_t ByteSize() const {return bytes;}
  size_t EntryCount() const {return entries.size();}
  Statistics Stats() const {return statistics;}
};
}
