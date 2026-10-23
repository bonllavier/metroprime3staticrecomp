#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace PrimeDlVertices {
// Local to one display-list visitor. Every non-draw command and nested list
// invalidates this proof; it is never a global mirror of mutable GX state.
struct PublicationTracker {
    bool valid=false;
    uint32_t format=0;
    bool NeedsPublish(uint32_t requested) const {return !valid || requested!=format;}
    void Completed(uint32_t requested){valid=true;format=requested;}
    void Invalidate(){valid=false;}
};
struct Attribute {
    uint32_t bytes=0, indexBytes=0, groups=1;
    uint32_t arrayBase=0, arrayStride=0;
    // Optional contiguous span proven by the caller for the entire index
    // domain. It is local to this expansion, never a cross-draw cache.
    const uint8_t* arrayData=nullptr;
    size_t arrayBytes=0;
};
inline void CopyComponents(uint8_t* destination,const uint8_t* source,size_t bytes) {
    // GX component widths are small and known. Constant-size copies avoid
    // calling the general CRT memmove for every indexed attribute component.
    switch(bytes) {
    case 1: std::memcpy(destination,source,1);break;
    case 2: std::memcpy(destination,source,2);break;
    case 3: std::memcpy(destination,source,3);break;
    case 4: std::memcpy(destination,source,4);break;
    case 6: std::memcpy(destination,source,6);break;
    case 8: std::memcpy(destination,source,8);break;
    case 9: std::memcpy(destination,source,9);break;
    case 12: std::memcpy(destination,source,12);break;
    case 24: std::memcpy(destination,source,24);break;
    case 36: std::memcpy(destination,source,36);break;
    default: std::memcpy(destination,source,bytes);break;
    }
}
template<unsigned IndexBytes,size_t CopyBytes=0>
inline void ExpandMappedAttributeImpl(const uint8_t* source,size_t sourceStride,
                                  uint8_t* destination,size_t directStride,
                                  uint32_t count,const Attribute& attribute) {
    for(uint32_t vertex=0;vertex<count;++vertex) {
        const uint8_t* components=source;
        if constexpr(IndexBytes!=0) {
            uint32_t index=source[0];
            if constexpr(IndexBytes==2)index=(index<<8)|source[1];
            components=attribute.arrayData+size_t(index)*attribute.arrayStride;
        }
        if constexpr(CopyBytes!=0)std::memcpy(destination,components,CopyBytes);
        else CopyComponents(destination,components,attribute.bytes);
        // Attribute starts may be inside the last vertex. Do not form a
        // pointer past the enclosing source/output allocation after it.
        if(vertex+1<count) {source+=sourceStride;destination+=directStride;}
    }
}
template<unsigned IndexBytes>
inline void ExpandMappedAttribute(const uint8_t* source,size_t sourceStride,
                                  uint8_t* destination,size_t directStride,
                                  uint32_t count,const Attribute& attribute) {
    // Dispatch common component widths once per attribute, rather than a
    // runtime copy-width jump table for every vertex. Other widths retain the
    // exact generic copy routine.
    switch(attribute.bytes) {
    case 4: return ExpandMappedAttributeImpl<IndexBytes,4>(source,sourceStride,destination,directStride,count,attribute);
    case 6: return ExpandMappedAttributeImpl<IndexBytes,6>(source,sourceStride,destination,directStride,count,attribute);
    case 8: return ExpandMappedAttributeImpl<IndexBytes,8>(source,sourceStride,destination,directStride,count,attribute);
    case 12: return ExpandMappedAttributeImpl<IndexBytes,12>(source,sourceStride,destination,directStride,count,attribute);
    default: return ExpandMappedAttributeImpl<IndexBytes>(source,sourceStride,destination,directStride,count,attribute);
    }
}
inline void ExpandMappedRange(const uint8_t* source,size_t sourceStride,
                              uint8_t* destination,size_t directStride,
                              uint32_t first,uint32_t last,
                              const std::vector<Attribute>& attributes) {
    size_t sourceOffset=0,directOffset=0;
    for(const auto& attribute:attributes) {
        for(uint32_t vertex=first;vertex<last;++vertex) {
            const uint8_t* input=source+size_t(vertex)*sourceStride+sourceOffset;
            const uint8_t* components=input;
            if(attribute.indexBytes) {
                uint32_t index=input[0];
                if(attribute.indexBytes==2)index=(index<<8)|input[1];
                components=attribute.arrayData+size_t(index)*attribute.arrayStride;
            }
            CopyComponents(destination+size_t(vertex)*directStride+directOffset,
                           components,attribute.bytes);
        }
        sourceOffset+=attribute.indexBytes ? attribute.indexBytes : attribute.bytes;
        directOffset+=attribute.bytes;
    }
}

// The draw still waits for expansion to finish before it is submitted to GX.
// Two persistent workers only help large, fully mapped packets; smaller draws
// stay on the current thread to avoid paying synchronization cost per primitive.
class MappedExpansionWorkers {
public:
    static MappedExpansionWorkers& Instance() {
        static MappedExpansionWorkers workers;
        return workers;
    }
    bool Run(const uint8_t* source,size_t sourceStride,uint8_t* destination,
             size_t directStride,uint32_t count,
             const std::vector<Attribute>& attributes) {
        if(m_workers.empty())return false;
        std::lock_guard<std::mutex> runLock(m_runMutex);
        Job job{source,sourceStride,destination,directStride,count,&attributes};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_job=&job;
            m_nextTask.store(0,std::memory_order_relaxed);
            m_finishedWorkers=0;
            ++m_generation;
        }
        m_start.notify_all();
        RunAvailableTasks(&job);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock,[this]{return m_finishedWorkers==m_workers.size();});
        m_job=nullptr;
        return true;
    }
private:
    struct Job {
        const uint8_t* source;
        size_t sourceStride;
        uint8_t* destination;
        size_t directStride;
        uint32_t count;
        const std::vector<Attribute>* attributes;
    };
    static constexpr uint32_t kVerticesPerTask=256;
    MappedExpansionWorkers() {
        const unsigned logical=std::thread::hardware_concurrency();
        const unsigned workerCount=logical>2 ? std::min(2u,logical-2u) : 0u;
        try {
            m_workers.reserve(workerCount);
            for(unsigned i=0;i<workerCount;++i)
                m_workers.emplace_back([this]{WorkerLoop();});
        } catch(...) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stopping=true;
            }
            m_start.notify_all();
            for(auto& worker:m_workers)if(worker.joinable())worker.join();
            m_workers.clear();
            m_stopping=false;
        }
    }
    ~MappedExpansionWorkers() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping=true;
        }
        m_start.notify_all();
        for(auto& worker:m_workers)if(worker.joinable())worker.join();
    }
    void RunAvailableTasks(const Job* job) {
        const uint32_t taskCount=1u+(job->count-1u)/kVerticesPerTask;
        for(;;) {
            const uint32_t task=m_nextTask.fetch_add(1,std::memory_order_relaxed);
            if(task>=taskCount)break;
            const uint32_t first=task*kVerticesPerTask;
            const uint32_t last=std::min(job->count,first+kVerticesPerTask);
            ExpandMappedRange(job->source,job->sourceStride,job->destination,
                              job->directStride,first,last,*job->attributes);
        }
    }
    void WorkerLoop() {
        uint64_t observedGeneration=0;
        std::unique_lock<std::mutex> lock(m_mutex);
        while(!m_stopping) {
            m_start.wait(lock,[this,observedGeneration]{
                return m_stopping || m_generation!=observedGeneration;
            });
            if(m_stopping)return;
            observedGeneration=m_generation;
            const Job* job=m_job;
            lock.unlock();
            RunAvailableTasks(job);
            lock.lock();
            ++m_finishedWorkers;
            if(m_finishedWorkers==m_workers.size())m_done.notify_one();
        }
    }
    std::vector<std::thread> m_workers;
    std::mutex m_runMutex,m_mutex;
    std::condition_variable m_start,m_done;
    std::atomic<uint32_t> m_nextTask{0};
    const Job* m_job=nullptr;
    uint64_t m_generation=0;
    size_t m_finishedWorkers=0;
    bool m_stopping=false;
};

// Expand array indices by copying the ORIGINAL big-endian component bytes.
// No float conversion, quantization, native GX attribute writer or stale array
// memo is involved. The caller publishes the corresponding direct VAT layout.
template<class ReadSpan>
bool Expand(const uint8_t* source,size_t sourceBytes,uint32_t count,
            const std::vector<Attribute>& attributes,std::vector<uint8_t>& output,
            ReadSpan&& read,size_t maxBytes,bool* usedParallel=nullptr) {
    if(usedParallel)*usedParallel=false;
    size_t sourceStride=0,directStride=0;
    for(const auto& a:attributes) {
        if(!a.bytes || !a.groups || a.bytes%a.groups ||
           (a.indexBytes!=0 && a.indexBytes!=1 && a.indexBytes!=2))return false;
        if(a.indexBytes && (!a.arrayBase || !a.arrayStride))return false;
        const uint64_t encoded=a.indexBytes ? uint64_t(a.indexBytes)*a.groups : a.bytes;
        if(encoded>maxBytes-sourceStride || a.bytes>maxBytes-directStride)return false;
        sourceStride+=encoded;directStride+=a.bytes;
    }
    if(!source || !count || !sourceStride || !directStride ||
       count>maxBytes/sourceStride || count>maxBytes/directStride || sourceBytes!=sourceStride*count)return false;
    output.resize(directStride*count);
    // Prove the ENTIRE encoded index domain before using the strided path.
    // This removes repeated address/span/group checks from individual vertices
    // without relying on the particular indices seen in an earlier draw.
    bool fullyMapped=true;
    for(const auto& a:attributes) {
        if(a.groups!=1) {fullyMapped=false;break;}
        if(!a.indexBytes)continue;
        const uint64_t last=a.indexBytes==1 ? 255u : 65535u;
        const uint64_t span=last*a.arrayStride+a.bytes;
        if(!a.arrayData || span>a.arrayBytes ||
           uint64_t(a.arrayBase)+span>uint64_t(UINT32_MAX)+1u) {
            fullyMapped=false;break;
        }
    }
    if(fullyMapped) {
        const bool hasIndexed=std::any_of(attributes.begin(),attributes.end(),
                                          [](const Attribute& a){return a.indexBytes!=0;});
        constexpr size_t kParallelMinimumBytes=64u*1024u;
        constexpr uint32_t kParallelMinimumVertices=512;
        if(hasIndexed && output.size()>=kParallelMinimumBytes &&
           count>=kParallelMinimumVertices &&
           MappedExpansionWorkers::Instance().Run(source,sourceStride,output.data(),
                                                  directStride,count,attributes)) {
            if(usedParallel)*usedParallel=true;
        } else {
            size_t sourceOffset=0,directOffset=0;
            for(const auto& a:attributes) {
                const auto* input=source+sourceOffset;
                auto* destination=output.data()+directOffset;
                if(a.indexBytes==2)ExpandMappedAttribute<2>(input,sourceStride,destination,directStride,count,a);
                else if(a.indexBytes==1)ExpandMappedAttribute<1>(input,sourceStride,destination,directStride,count,a);
                else ExpandMappedAttribute<0>(input,sourceStride,destination,directStride,count,a);
                sourceOffset+=a.indexBytes ? a.indexBytes : a.bytes;
                directOffset+=a.bytes;
            }
        }
        return true;
    }
    size_t input=0,destination=0;
    for(uint32_t vertex=0;vertex<count;++vertex)for(const auto& a:attributes) {
        if(!a.indexBytes) {
            CopyComponents(output.data()+destination,source+input,a.bytes);
            input+=a.bytes;destination+=a.bytes;continue;
        }
        // Almost all attributes have one group. Avoid integer division in
        // every vertex of that common case; NBT3 retains its three groups.
        const size_t groupBytes=a.groups==1 ? a.bytes : a.bytes/a.groups;
        for(uint32_t group=0;group<a.groups;++group) {
            uint32_t index=source[input++];
            if(a.indexBytes==2)index=(index<<8)|source[input++];
            const uint64_t address=uint64_t(a.arrayBase)+uint64_t(index)*a.arrayStride;
            if(address>UINT32_MAX || groupBytes>uint64_t(UINT32_MAX)+1-address)return false;
            const uint8_t* span=nullptr;
            const uint64_t offset=uint64_t(index)*a.arrayStride;
            if(a.arrayData && offset<=a.arrayBytes && groupBytes<=a.arrayBytes-offset)
                span=a.arrayData+offset;
            else if(!read(uint32_t(address),groupBytes,span) || !span)return false;
            CopyComponents(output.data()+destination,span,groupBytes);destination+=groupBytes;
        }
    }
    return input==sourceBytes && destination==output.size();
}
} // namespace PrimeDlVertices
