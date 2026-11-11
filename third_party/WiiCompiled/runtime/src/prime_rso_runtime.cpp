#include "hle/prime_rso_runtime.h"
#if defined(MKW_PRIME_RSO_AVAILABLE)
#include "hle/prime_rso_relocations.h"
#include "abi_bridge.h"
#include "memory.h"
#include "runtime_log.h"
#include "hle/prime_video_trace.h"
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <bcrypt.h>

// Title / file-select front end (its RSO is bound while the menus run): host
// overlays that belong to gameplay stay hidden meanwhile.
namespace { std::atomic<int> g_primeFrontEndModules{0}; }
bool PrimeFrontEndActive() { return g_primeFrontEndModules.load(std::memory_order_relaxed) > 0; }

extern const PrimeRsoModule* const g_primeRuntimeRsoModules[];
extern const size_t g_primeRuntimeRsoModuleCount;

namespace PrimeRsoRuntime {
using namespace PrimeRsoRelocations;
struct Binding {
    const PrimeRsoModule* module = nullptr;
    uint32_t base = 0;
    uint32_t epilog = 0;
    std::vector<uint32_t> sections;
    std::unordered_map<uint32_t, RawDispatchRecord> functions;
    std::unordered_map<uint32_t, uint32_t> thunks;
    std::unordered_map<uint32_t, uint32_t> hits;
    uint32_t calls = 0;
};
std::unordered_map<uint32_t, std::unique_ptr<Binding>> bindings;
bool GuestRam(uint32_t address) {
    return (address >= 0x80000000u && address < 0x81800000u) ||
        (address >= 0x90000000u && address < 0x98000000u);
}

std::string Sha256(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("Cannot initialize RSO validation hash");
    std::array<uint8_t, 32> digest{};
    BCRYPT_HASH_HANDLE hash = nullptr;
    ULONG objectSize = 0, resultSize = 0;
    auto status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0);
    std::vector<uint8_t> object(objectSize);
    if (status == 0) status = BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0);
    if (status == 0) status = BCryptHashData(hash, const_cast<uint8_t*>(bytes.data()),
        static_cast<ULONG>(bytes.size()), 0);
    if (status == 0) status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) throw std::runtime_error("RSO validation hash failed");
    std::string text;
    constexpr char hex[] = "0123456789abcdef";
    for (uint8_t value : digest) { text += hex[value >> 4]; text += hex[value & 15]; }
    return text;
}

uint32_t Address(const Binding& value, uint32_t nominal) {
    for (size_t i = 0; i < value.module->sectionCount; ++i) {
        const auto& section = value.module->sections[i];
        uint32_t address = 0;
        if (MapSection(nominal, section.nominal, value.sections[i], section.size, false, address)) return address;
    }
    throw std::runtime_error("Nominal RSO address is outside verified sections");
}

bool VerifyThunk(Binding& value, uint32_t address, uint32_t expected) {
    if (!Memory::Contains(address, 24u) || !GuestRam(address)) return false;
    std::array<uint32_t, 6> words{};
    for (uint32_t i = 0; i < words.size(); ++i) words[i] = Memory::Read32(address + i * 4u);
    uint32_t target = 0;
    if (!DecodeSdkThunk(words, target) || target != expected) return false;
    value.thunks[address] = target;
    return true;
}

std::unique_ptr<Binding> Bind(uint32_t prolog, const PrimeRsoModule& module) {
    // The descriptor pins the original file placement; BSS remains independent.
    const uint32_t base = prolog - module.prologSectionFileOffset - module.prologOffset;
    if (!GuestRam(base) || !Memory::Contains(base, module.originalSize)) return {};
    if (Memory::Read32(base + 0x24u) != prolog || Memory::Read32(base + 8u) != module.sectionCount ||
        Memory::Read32(base + 0x18u) != 1u) return {};
    const uint32_t table = Memory::Read32(base + 0xCu);
    if (!Memory::Contains(table, module.sectionCount * 8u)) return {};
    auto value = std::make_unique<Binding>();
    value->module = &module;
    value->base = base;
    value->epilog = Memory::Read32(base + 0x28u);
    std::vector<std::vector<uint8_t>> masked(module.sectionCount);
    for (size_t i = 0; i < module.sectionCount; ++i) {
        const uint32_t address = Memory::Read32(table + static_cast<uint32_t>(i * 8u));
        const uint32_t size = Memory::Read32(table + static_cast<uint32_t>(i * 8u + 4u));
        if (size != module.sections[i].size || (size && !Memory::Contains(address, size))) return {};
        value->sections.push_back(address);
        masked[i].resize(size);
        for (uint32_t offset = 0; offset < size; ++offset) masked[i][offset] = Memory::Read8(address + offset);
    }
    if (value->sections[module.prologSection] + module.prologOffset != prolog) return {};
    for (size_t i = 0; i < module.relocationCount; ++i) {
        const auto& relocation = module.relocations[i];
        const uint32_t source = value->sections.at(relocation.section) + relocation.offset;
        const uint32_t target = relocation.external ? relocation.importedTarget + relocation.addend :
            value->sections.at(relocation.targetSection) + relocation.addend;
        auto& bytes = masked.at(relocation.section);
        if (relocation.type == 1u) {
            const uint32_t actual = Memory::Read32(source);
            if (actual != target && !(relocation.external && VerifyThunk(*value, actual, target)))
                throw std::runtime_error("RSO ADDR32 differs from the verified SDK relocation");
            for (uint32_t j = 0; j < 4u; ++j) bytes.at(relocation.offset + j) = 0;
        } else if (relocation.type == 4u || relocation.type == 6u) {
            const uint16_t expected = relocation.type == 4u ? static_cast<uint16_t>(target) : HighAdjusted(target);
            if (Memory::Read16(source) != expected)
                throw std::runtime_error("RSO LO/HA differs from the verified SDK relocation");
            bytes.at(relocation.offset) = bytes.at(relocation.offset + 1u) = 0;
        } else if (relocation.type == 10u) {
            const uint32_t instruction = Memory::Read32(source);
            const uint32_t destination = BranchTarget(instruction, source);
            if ((instruction >> 26u) != 18u || (instruction & 2u) ||
                (destination != target && !(relocation.external && VerifyThunk(*value, destination, target))))
                throw std::runtime_error("RSO REL24 differs from the verified SDK relocation");
            bytes.at(relocation.offset) &= 0xFCu;
            bytes.at(relocation.offset + 1u) = bytes.at(relocation.offset + 2u) = 0;
            bytes.at(relocation.offset + 3u) &= 3u;
        } else throw std::runtime_error("Unsupported runtime RSO relocation");
    }
    for (size_t i = 0; i < module.sectionCount; ++i) {
        if (!masked[i].empty() && Sha256(masked[i]) != module.sections[i].maskedSha256)
            throw std::runtime_error("RSO section bytes differ outside the verified relocation fields");
    }
    for (size_t i = 0; i < module.functionCount; ++i) {
        const auto& function = module.functions[i];
        const uint32_t address = Address(*value, function.nominal);
        value->functions.emplace(address, RawDispatchRecord{address, function.entry, 0u, false});
    }
    if (!value->functions.count(value->epilog))
        throw std::runtime_error("SDK RSO epilog is outside the verified translated entries");
    if (std::strcmp(module.name, "ScriptGUIScreenFrontEndUI") == 0) g_primeFrontEndModules.fetch_add(1, std::memory_order_relaxed);
    RT_LOG(RT_TAG_RUNTIME) << "[prime-rso-bind] module=" << module.name << " base=0x" << std::hex << base
        << " prolog=0x" << prolog << std::dec << " relocations=" << module.relocationCount
        << " functions=" << module.functionCount << " thunks=" << value->thunks.size()
        << " section_hashes_verified=1" << std::endl;
    return value;
}

Binding& Get(uint32_t moduleId) {
    const auto found = bindings.find(moduleId);
    if (found == bindings.end()) throw std::runtime_error("Runtime RSO is not bound");
    return *found->second;
}
} // namespace PrimeRsoRuntime

extern "C" uint32_t PPC_RsoRuntimeAddress(uint32_t moduleId, uint32_t nominal) {
    return PrimeRsoRuntime::Address(PrimeRsoRuntime::Get(moduleId), nominal);
}
extern "C" uint32_t PPC_RsoNominalAddress(uint32_t moduleId, uint32_t runtime) {
    const auto& value = PrimeRsoRuntime::Get(moduleId);
    for (size_t i = 0; i < value.module->sectionCount; ++i) {
        const auto& section = value.module->sections[i];
        uint32_t nominal = 0;
        if (PrimeRsoRelocations::MapSection(runtime, section.nominal, value.sections[i], section.size, true, nominal)) return nominal;
    }
    return runtime;
}
extern "C" uint32_t PPC_RsoReadImmediate(uint32_t moduleId, uint32_t instruction, uint32_t flags) {
    if (flags != 1u && flags != 3u) throw std::runtime_error("Unsupported runtime RSO immediate representation");
    const uint32_t address = PPC_RsoRuntimeAddress(moduleId, instruction);
    return PrimeRsoRelocations::SignedImmediate(Memory::Read32(address), (flags & 2u) != 0);
}
extern "C" void PPC_RsoImportCall(CpuContext* cpu, uint32_t moduleId, uint32_t instruction) {
    const uint32_t address = PPC_RsoRuntimeAddress(moduleId, instruction);
    const uint32_t branch = Memory::Read32(address);
    if ((branch >> 26u) != 18u || (branch & 2u)) throw std::runtime_error("Invalid runtime RSO import branch");
    const bool movieQuery=moduleId==3 && (instruction==0x806AB6D8u || instruction==0x806AB6F8u || instruction==0x806AB718u)
        && std::getenv("WII_COMPILED_TRACE_PRIME_MOVIE_CONTROL");
    static uint32_t movieQueryCount=0;
    const bool traceQuery=movieQuery && (++movieQueryCount<=12 || (movieQueryCount<=6000 && movieQueryCount%120==0));
    const uint32_t actor=traceQuery ? cpu->gpr[3] : 0;
    InvokeIndirectCpu(PrimeRsoRelocations::BranchTarget(branch, address), cpu);
    if(traceQuery) RT_LOG(RT_TAG_RUNTIME)<<"[prime-front-query] source=0x"<<std::hex<<instruction
        <<" target=0x"<<PrimeRsoRelocations::BranchTarget(branch,address)<<" actor=0x"<<actor
        <<" result=0x"<<cpu->gpr[3]<<" r29=0x"<<cpu->gpr[29]<<" r30=0x"<<cpu->gpr[30]<<" r31=0x"<<cpu->gpr[31]<<std::dec<<std::endl;
}

namespace PrimeRsoRuntime {
// Calls into RSO module code are frequent (much of the game logic lives in modules): every one used
// to scan each loaded module's function and thunk maps, look the function up again and bump a
// per-target hit counter. A direct-mapped memo of the resolved (binding, record) pair skips all of
// that after the first call of a target. Any change to the binding set (a module bound, replaced or
// retired) bumps the generation and invalidates every entry; module epilogs are never memoised, and
// the two per-call traces (front-end bind, movie gate) are kept as flags.
struct RsoCallMemo {
    uint32_t target = 0;
    uint32_t generation = 0;
    Binding* binding = nullptr;
    const RawDispatchRecord* record = nullptr;
    uint8_t flags = 0;
};
constexpr uint8_t kRsoMemoFrontEnd = 1, kRsoMemoMovieGate = 2;
constexpr unsigned kRsoMemoBits = 10;
std::array<RsoCallMemo, 1u << kRsoMemoBits> g_rsoCallMemo{};
uint32_t g_rsoBindingGeneration = 1;
inline RsoCallMemo& RsoMemoSlot(uint32_t target) {
    return g_rsoCallMemo[((target >> 2) * 2654435761u) >> (32 - kRsoMemoBits)];
}
void TraceMovieGate(const CpuContext* cpu) {
    if (!std::getenv("WII_COMPILED_TRACE_PRIME_MOVIE_CONTROL")) return;
    static uint32_t gateCalls=0;
    const uint32_t n=gateCalls++;
    if(n<8 || (n<4800 && n%120==0)) RT_LOG(RT_TAG_RUNTIME)<<"[prime-front-gate] call="<<n<<" result="<<cpu->gpr[3]<<std::endl;
}
}  // namespace PrimeRsoRuntime

bool PRIME_RSO_TryInvoke(uint32_t target, CpuContext* cpu) {
    using namespace PrimeRsoRuntime;
    if (!cpu || !GuestRam(target)) return false;
    if (auto& memo = RsoMemoSlot(target); memo.target == target && memo.generation == g_rsoBindingGeneration) {
        struct Guard { Binding& value; Guard(Binding& b) : value(b) { ++value.calls; } ~Guard() { --value.calls; } } guard(*memo.binding);
        const uint8_t flags = memo.flags;
        if (flags & kRsoMemoFrontEnd) PrimeTraceFrontEnd(cpu);
        const bool dispatched = TryDispatchRawCpuTarget(memo.record, cpu);
        if (flags & kRsoMemoMovieGate) TraceMovieGate(cpu);
        return dispatched;
    }
    Binding* binding = nullptr;
    for (const auto& item : bindings) {
        if (item.second->functions.count(target) || item.second->thunks.count(target)) {
            binding = item.second.get();
            break;
        }
    }
    if (!binding) {
        for (size_t moduleIndex = 0; moduleIndex < g_primeRuntimeRsoModuleCount; ++moduleIndex) {
            const auto* module = g_primeRuntimeRsoModules[moduleIndex];
            auto candidate = Bind(target, *module);
            if (!candidate) continue;
            auto previous = bindings.find(module->id);
            if (previous != bindings.end() && previous->second->calls)
                throw std::runtime_error("Cannot replace an RSO binding while its host code is active");
            binding = candidate.get();
            bindings[module->id] = std::move(candidate);
            ++g_rsoBindingGeneration;
            break;
        }
        if (!binding) return false;
    }
    const auto function = binding->functions.find(target);
    if (function != binding->functions.end()) {
        const bool epilog = target == binding->epilog;
        const auto* module = binding->module;
        bool dispatched = false;
        {
        struct Guard { Binding& value; Guard(Binding& b) : value(b) { ++value.calls; } ~Guard() { --value.calls; } } guard(*binding);
        if (epilog && binding->calls != 1)
            throw std::runtime_error("Cannot unload an RSO while another translated module frame is active");
        const bool dolCaller = cpu->lr >= 0x80000000u && cpu->lr < 0x80590000u;
        const bool frontEndBind = module->id==3 && PPC_RsoNominalAddress(3,target)==0x806AB8BCu;
        const bool movieGate = module->id==3 && PPC_RsoNominalAddress(3,target)==0x806AB6A0u;
        if (!epilog) {
            // Filled before the call: a binding change inside it bumps the generation and drops it.
            {
                auto& memo = RsoMemoSlot(target);
                memo = {target, g_rsoBindingGeneration, binding, &function->second,
                        static_cast<uint8_t>((frontEndBind ? kRsoMemoFrontEnd : 0) | (movieGate ? kRsoMemoMovieGate : 0))};
            }
        }
        if(frontEndBind) PrimeTraceFrontEnd(cpu);
        const bool trace = binding->hits[target]++ == 0 && (binding->hits.size() <= 32 || dolCaller);
        if (trace) RT_LOG(RT_TAG_RUNTIME) << "[prime-rso-enter] target=0x" << std::hex << target
            << " nominal=0x" << PPC_RsoNominalAddress(binding->module->id, target)
            << " sp=0x" << cpu->gpr[1] << " lr=0x" << cpu->lr << std::dec << std::endl;
        dispatched = TryDispatchRawCpuTarget(&function->second, cpu);
        if(movieGate) TraceMovieGate(cpu);
        if (trace) RT_LOG(RT_TAG_RUNTIME) << "[prime-rso-return] target=0x" << std::hex << target
            << " sp=0x" << cpu->gpr[1] << " lr=0x" << cpu->lr << " r3=0x" << cpu->gpr[3]
            << std::dec << " dispatched=" << dispatched << std::endl;
        }
        if (epilog) {
            bindings.erase(module->id);
            ++g_rsoBindingGeneration;
            if (std::strcmp(module->name, "ScriptGUIScreenFrontEndUI") == 0 && g_primeFrontEndModules.load() > 0)
                g_primeFrontEndModules.fetch_sub(1, std::memory_order_relaxed);
            RT_LOG(RT_TAG_RUNTIME) << "[prime-rso-retire] module=" << module->name
                << " reason=original_epilog_return active_frames=0" << std::endl;
        }
        return dispatched;
    }
    const auto thunk = binding->thunks.find(target);
    if (thunk != binding->thunks.end()) {
        CpuContextScope scope(cpu);
        const uint32_t savedR12 = cpu->gpr[12];
        Memory::Write32(cpu->gpr[1] + 4u, savedR12);
        cpu->gpr[12] = thunk->second;
        cpu->ctr = cpu->gpr[12];
        cpu->gpr[12] = Memory::Read32(cpu->gpr[1] + 4u);
        InvokeIndirectCpu(cpu->ctr, cpu);
        return true;
    }
    return false;
}
#else
bool PRIME_RSO_TryInvoke(uint32_t, CpuContext*) { return false; }
#endif
