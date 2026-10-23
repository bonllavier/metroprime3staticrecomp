#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <map>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <Windows.h>
#include <TlHelp32.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

// Read-only host counters, sampled at most once a second from the UI thread.
// Thread percent is relative to ONE logical processor; process percent uses
// all logical processors, matching Task Manager's process normalization.
namespace HostPerformance {
struct Thread {uint32_t id=0;double percent=0;};
struct Snapshot {
    bool ready=false,gpuReady=false;
    unsigned logicalProcessors=0;
    double processPercent=0,busyLogicalProcessors=0,gpuPercent=0;
    std::string gpuEngine;
    std::vector<Thread> threads;
};
class Sampler {
    Snapshot result;
    std::chrono::steady_clock::time_point last{};
#if defined(_WIN32)
    uint64_t previousProcess=0;
    std::map<DWORD,uint64_t> previousThreads;
    HMODULE pdh=nullptr;
    PDH_HQUERY query=nullptr;
    PDH_HCOUNTER gpuCounter=nullptr;
    decltype(&PdhOpenQueryW) openQuery=nullptr;
    decltype(&PdhAddEnglishCounterW) addCounter=nullptr;
    decltype(&PdhCollectQueryData) collect=nullptr;
    decltype(&PdhGetFormattedCounterArrayW) readArray=nullptr;
    decltype(&PdhCloseQuery) closeQuery=nullptr;
    bool gpuPrimed=false;
    static uint64_t ticks(FILETIME f) {return (uint64_t(f.dwHighDateTime)<<32)|f.dwLowDateTime;}
    static uint64_t cpuTime(HANDLE h,bool process) {
        FILETIME created{},exited{},kernel{},user{};
        if(!(process ? GetProcessTimes(h,&created,&exited,&kernel,&user) :
                       GetThreadTimes(h,&created,&exited,&kernel,&user)))return 0;
        return ticks(kernel)+ticks(user);
    }
    void InitGpu() {
        pdh=LoadLibraryExW(L"pdh.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!pdh)return;
        openQuery=reinterpret_cast<decltype(openQuery)>(GetProcAddress(pdh,"PdhOpenQueryW"));
        addCounter=reinterpret_cast<decltype(addCounter)>(GetProcAddress(pdh,"PdhAddEnglishCounterW"));
        collect=reinterpret_cast<decltype(collect)>(GetProcAddress(pdh,"PdhCollectQueryData"));
        readArray=reinterpret_cast<decltype(readArray)>(GetProcAddress(pdh,"PdhGetFormattedCounterArrayW"));
        closeQuery=reinterpret_cast<decltype(closeQuery)>(GetProcAddress(pdh,"PdhCloseQuery"));
        if(!openQuery || !addCounter || !collect || !readArray || !closeQuery)return;
        if(openQuery(nullptr,0,&query)!=ERROR_SUCCESS)return;
        wchar_t path[160];
        swprintf(path,160,L"\\GPU Engine(pid_%lu_*)\\Utilization Percentage",GetCurrentProcessId());
        if(addCounter(query,path,0,&gpuCounter)!=ERROR_SUCCESS)gpuCounter=nullptr;
    }
    void SampleGpu() {
        result.gpuReady=false;
        if(!gpuCounter || collect(query)!=ERROR_SUCCESS)return;
        if(!gpuPrimed){gpuPrimed=true;return;}
        DWORD bytes=0,count=0;
        if(readArray(gpuCounter,PDH_FMT_DOUBLE,&bytes,&count,nullptr)!=PDH_MORE_DATA || !bytes || bytes>1024*1024)return;
        // Aligned storage for the provider's array and instance strings.
        std::vector<uint64_t> storage((bytes+7)/8);
        auto* items=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
        if(readArray(gpuCounter,PDH_FMT_DOUBLE,&bytes,&count,items)!=ERROR_SUCCESS)return;
        double maximum=-1;
        result.gpuEngine.clear();
        for(DWORD i=0;i<count;++i)if(items[i].FmtValue.CStatus==PDH_CSTATUS_VALID_DATA ||
                                     items[i].FmtValue.CStatus==PDH_CSTATUS_NEW_DATA) {
            if(items[i].FmtValue.doubleValue>maximum) {
                maximum=items[i].FmtValue.doubleValue;
                const auto* engine=std::wcsstr(items[i].szName,L"engtype_");
                if(engine) {
                    engine+=8;char name[100]{};
                    WideCharToMultiByte(CP_UTF8,0,engine,-1,name,sizeof(name),nullptr,nullptr);
                    result.gpuEngine=name;
                }
            }
            result.gpuReady=true;
        }
        result.gpuPercent=std::clamp(maximum,0.0,100.0);
    }
#endif
public:
    Sampler() {
#if defined(_WIN32)
        SYSTEM_INFO info{};GetSystemInfo(&info);result.logicalProcessors=info.dwNumberOfProcessors;
        InitGpu();
#endif
    }
    ~Sampler() {
#if defined(_WIN32)
        if(query && closeQuery)closeQuery(query);
        if(pdh)FreeLibrary(pdh);
#endif
    }
    Sampler(const Sampler&)=delete;
    const Snapshot& Poll() {
        const auto now=std::chrono::steady_clock::now();
        const double elapsed=std::chrono::duration<double>(now-last).count();
        const bool first=last==std::chrono::steady_clock::time_point{};
        if(!first && elapsed<1)return result;
#if defined(_WIN32)
        const uint64_t process=cpuTime(GetCurrentProcess(),true);
        if(!first && process>=previousProcess && result.logicalProcessors) {
            result.busyLogicalProcessors=double(process-previousProcess)/1e7/elapsed;
            result.processPercent=100*result.busyLogicalProcessors/result.logicalProcessors;
            result.ready=true;
        }
        previousProcess=process;result.threads.clear();
        std::map<DWORD,uint64_t> next;
        const HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
        if(snapshot!=INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
            for(BOOL ok=Thread32First(snapshot,&entry);ok;ok=Thread32Next(snapshot,&entry)) {
                if(entry.th32OwnerProcessID!=GetCurrentProcessId())continue;
                HANDLE thread=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID);
                if(!thread)continue;
                const uint64_t current=cpuTime(thread,false);CloseHandle(thread);
                next[entry.th32ThreadID]=current;
                const auto old=previousThreads.find(entry.th32ThreadID);
                if(!first && old!=previousThreads.end() && current>=old->second)
                    result.threads.push_back({entry.th32ThreadID,100*double(current-old->second)/1e7/elapsed});
            }
            CloseHandle(snapshot);
        }
        previousThreads=std::move(next);
        std::sort(result.threads.begin(),result.threads.end(),[](const auto& a,const auto& b){return a.percent>b.percent;});
        SampleGpu();
#endif
        last=now;return result;
    }
};
}
