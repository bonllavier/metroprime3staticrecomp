#pragma once

// Experimental host-stack transport. It does not select guest threads, edit
// guest queues or restore OSContext. Integration requires the original SDK
// save/load continuations. Hardware timing and full thread lifetime semantics
// require independent validation beyond these host-stack contracts.
#include "host_context.h"
#include "guest_abi/cpu_context.h"
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

class PrimeHostContinuations {
public:
    using Bootstrap = std::function<void(uint32_t, CpuContext*)>;
    explicit PrimeHostContinuations(Bootstrap bootstrap) : bootstrap_(std::move(bootstrap)) {}
    ~PrimeHostContinuations() {
        for (auto& [id, record] : records_) {
            if (id != root_ && record->handle) HostContext::Destroy(record->handle);
        }
    }
    PrimeHostContinuations(const PrimeHostContinuations&) = delete;
    PrimeHostContinuations& operator=(const PrimeHostContinuations&) = delete;

    void BindRoot(uint32_t id, CpuContext* cpu) {
        if (root_ || !id || !cpu) throw std::runtime_error("Invalid root continuation binding");
#if defined(_WIN32)
        if (!IsThreadAFiber()) throw std::runtime_error("Host thread is not a fiber");
        auto record = std::make_unique<Record>();
        record->handle = GetCurrentFiber();
        record->externalCpu = cpu;
        record->owner = this;
        record->id = id;
        records_.emplace(id, std::move(record));
        root_ = active_ = id;
#else
        throw std::runtime_error("Experimental Prime continuation transport requires Windows");
#endif
    }

    // Returns only when the source host stack is selected again. The caller
    // must then load its OWN guest OSContext, never the originally requested
    // target. Returning the source id makes this obligation explicit.
    uint32_t Transfer(uint32_t target, CpuContext* cpu) {
        if (!root_ || !target) throw std::runtime_error("Unbound continuation transfer");
        Record& source = *records_.at(active_);
        if (source.Cpu() != cpu || !HostContext::IsCurrent(source.handle))
            throw std::runtime_error("Continuation owner does not match active CPU/host stack");
        if (target == active_) return active_;
        auto it = records_.find(target);
        if (it == records_.end()) {
            auto record = std::make_unique<Record>();
            record->owner = this;
            record->id = target;
            record->cpu = *cpu;
            record->handle = HostContext::Create(4u * 1024u * 1024u, Enter, record.get());
            if (!record->handle) throw std::runtime_error("Cannot allocate Prime host continuation");
            it = records_.emplace(target, std::move(record)).first;
        }
        Record& destination = *it->second;
        if (destination.finished) throw std::runtime_error("Attempt to resume a finished continuation");
        Activate(destination);
        HostContext::Switch(destination.handle);
        if (active_ != source.id) throw std::runtime_error("Wrong host continuation resumed");
        Activate(source);
        if (failure_) std::rethrow_exception(failure_);
        return source.id;
    }
    uint32_t Active() const { return active_; }

private:
    struct Record {
        HostContext::Handle handle = nullptr;
        PrimeHostContinuations* owner = nullptr;
        CpuContext cpu{};
        CpuContext* externalCpu = nullptr;
        uint32_t id = 0;
        bool finished = false;
        CpuContext* Cpu() { return externalCpu ? externalCpu : &cpu; }
    };
    void Activate(Record& record) {
        active_ = record.id;
        g_currentCpuContext = record.Cpu();
        MkwApplyHostNiMode(record.Cpu()->fpscr);
    }
    static void Enter(void* pointer) {
        auto& record = *static_cast<Record*>(pointer);
        auto& owner = *record.owner;
        try {
            owner.bootstrap_(record.id, record.Cpu());
            throw std::runtime_error("Guest bootstrap returned without a guest exit continuation");
        } catch (...) {
            owner.failure_ = std::current_exception();
        }
        record.finished = true;
        auto& root = *owner.records_.at(owner.root_);
        owner.Activate(root);
        HostContext::Switch(root.handle);
        std::terminate();
    }
    Bootstrap bootstrap_;
    std::unordered_map<uint32_t, std::unique_ptr<Record>> records_;
    uint32_t root_ = 0;
    uint32_t active_ = 0;
    std::exception_ptr failure_;
};
