#pragma once

#include "texture.hpp"
#include <filesystem>
#include <optional>
#include <vector>

namespace aurora::gfx::texture_replacement {
void initialize() noexcept;
void shutdown() noexcept;
void register_tlut(const GXTlutObj* obj, const void* data, GXTlutFmt format, uint16_t entries) noexcept;
void load_tlut(const GXTlutObj* obj, uint32_t idx) noexcept;
std::optional<TextureHandle> find_replacement(const GXTexObj_& obj) noexcept;
// Replacement variants (<texture_replacements>/variants/<name>/, "" = none): request from any
// thread; the texture-resolving thread applies it and receives the replacement handles that are no
// longer valid, which it must forget from its own caches.
void request_variant(const char* name) noexcept;
bool apply_pending_variant(std::vector<TextureHandle>& dropped) noexcept;
// After a switch: whether a non-palette texture with this base level is one either variant replaces
// (its cached original conversion must be resolved again).
bool variant_changed_texture(uint32_t width, uint32_t height, uint32_t format, const void* data,
                             size_t baseLevelBytes) noexcept;
std::string build_texture_replacement_name(const GXTexObj_& obj) noexcept;
// Content hash of the base level, the one in replacement names.
uint64_t texture_content_hash(const GXTexObj_& obj) noexcept;

// Texture tags: host effects that skip surfaces drawn with a listed texture (content hashes).
// Each draw ORs the tags of its sampled textures (cached per TextureRef, see TextureRef::tagBits).
enum TextureTag : uint8_t {
  TagNoSsao = 1,   // Samus' face behind the visor: no ambient occlusion
  TagNoMetal = 2,  // skin, eyes: no enhanced-metal reflection
  // Transparent surfaces (Samus' visor glass) that write no depth: no SSAO wherever they change the
  // colour. Bit 5 (bits 2-4 and 6 hold the material class; bit 7 is the cache's "not hashed yet").
  TagSsaoCover = 0x20,
};
void set_texture_tag_list(uint8_t tag, const uint64_t* hashes, size_t count) noexcept;
// Enhanced materials: a texture can also belong to one material class (1..15), kept in the same tag
// byte: class bits 0-2 in bits 2-4, class bit 3 in bit 6. Replaces the textures of class `cls`.
constexpr uint8_t MaterialClassCount = 16;
constexpr uint8_t MaterialClassMask = 0x5C;
constexpr uint8_t material_class_bits(uint8_t cls) noexcept {
  return static_cast<uint8_t>(((cls & 7u) << 2) | ((cls & 8u) << 3));
}
inline uint8_t material_class_of(uint8_t tags) noexcept {
  return static_cast<uint8_t>(((tags >> 2) & 7u) | ((tags >> 3) & 8u));
}
void set_texture_material_list(uint8_t cls, const uint64_t* hashes, size_t count) noexcept;
uint8_t texture_tags_listed() noexcept;                  // tags that have at least one texture
uint8_t texture_tags(const GXTexObj_& obj) noexcept;    // hashes the texture
// Writes the texture as <dir>/<replacement name> (once) and returns that name; for F7 draw dumps.
std::string dump_texture_for_inspection(const GXTexObj_& obj, const std::filesystem::path& dir) noexcept;
} // namespace aurora::gfx::texture_replacement
