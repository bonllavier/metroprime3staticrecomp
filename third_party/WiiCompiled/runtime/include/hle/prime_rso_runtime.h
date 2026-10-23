#pragma once
#include <cstddef>
#include <cstdint>
struct CpuContext;

struct PrimeRsoSection { uint32_t nominal, size; const char* maskedSha256; };
struct PrimeRsoRelocation {
    uint32_t section, offset, type, targetSection, addend, importedTarget;
    bool external;
};
struct PrimeRsoFunction { uint32_t nominal; void (*entry)(CpuContext*); };
struct PrimeRsoModule {
    uint32_t id, originalSize, prologSection, prologOffset, prologSectionFileOffset;
    const char* name;
    const char* originalSha256;
    const PrimeRsoSection* sections;
    size_t sectionCount;
    const PrimeRsoRelocation* relocations;
    size_t relocationCount;
    const PrimeRsoFunction* functions;
    size_t functionCount;
};

extern const PrimeRsoModule g_primeRhbmModule;
extern const PrimeRsoModule g_primeTweaksModule;
extern const PrimeRsoModule g_primeFrontEndUiModule;
extern "C" uint32_t PPC_RsoRuntimeAddress(uint32_t moduleId, uint32_t nominal);
extern "C" uint32_t PPC_RsoNominalAddress(uint32_t moduleId, uint32_t runtime);
extern "C" uint32_t PPC_RsoReadImmediate(uint32_t moduleId, uint32_t instruction, uint32_t flags);
extern "C" void PPC_RsoImportCall(CpuContext* cpu, uint32_t moduleId, uint32_t instruction);
bool PRIME_RSO_TryInvoke(uint32_t target, CpuContext* cpu);
