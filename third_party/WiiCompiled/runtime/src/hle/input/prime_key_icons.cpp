// Keyboard/mouse icons for the game's button prompts (see hle/prime_key_icons.h).
#include "hle/prime_key_icons.h"

#include "hle/prime_key_bindings.h"
#include "hle/prime_gamepad_runtime.h"
#include "runtime_log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>  // PROPID and IStream for the GDI+ headers
using std::max;
using std::min;
#include <gdiplus.h>
#endif

namespace PrimeKeyIcons {
namespace {
// Texture table (RM3E01 disc paks, 2026-10-25): file = tex1_<w>x<h>_<xxh64 of the
// base level>_<GX format>. Several game textures share pixel data and therefore
// one file (B_Default = IconJumpButton, Rev_Minus = IconVisorSelectButton,
// Rev_Plus = IconHyperButton).
constexpr std::array kTable{
    // A button
    TextureSpec{"tex1_32x32_b0c321687c7a410b_5", 32, 32, "a", nullptr, nullptr, "A (menus, map: Pan/select)"},
    TextureSpec{"tex1_32x32_bb21e8755f36b2f0_5", 32, 32, "a", nullptr, nullptr, "AButtonIn (front end)"},
    TextureSpec{"tex1_32x32_47002754133e4ff5_6", 32, 32, "a", nullptr, nullptr, "TXTR_RevASmaller (options: Fire)"},
    TextureSpec{"tex1_32x32_173f3a24d9dd4a66_6", 32, 32, "a", nullptr, nullptr, "PRESS A TO START"},
    TextureSpec{"tex1_32x32_380a4ed102753a91_6", 32, 32, "a", nullptr, nullptr, "A pressed (animated frame)"},
    TextureSpec{"tex1_32x32_f00e0e837140250a_6", 32, 32, "a", nullptr, nullptr, "TXTR_IconFireButton (HUD prompts: Press A)"},
    TextureSpec{"tex1_32x32_4eb90bb511d2b10e_4", 32, 32, "a", nullptr, nullptr, "A, RGB565 (GuiDVD/GuiNAND menus)"},
    TextureSpec{"tex1_32x32_00901119431a3e6a_14", 32, 32, "a", nullptr, nullptr, "A, white outline (GuiNAND)"},
    TextureSpec{"tex1_32x32_fd38631c4cfd1732_14", 32, 32, "a", nullptr, nullptr, "A, teal (GuiNAND)"},
    // B button
    TextureSpec{"tex1_32x32_e6dbfd18d4666ee7_5", 32, 32, "b", nullptr, nullptr, "BButtonIn (Back)"},
    TextureSpec{"tex1_32x32_43be3d9c0c2e2dbd_6", 32, 32, "b", nullptr, nullptr, "TXTR_B_Default / TXTR_IconJumpButton"},
    TextureSpec{"tex1_32x32_ce9f9d8116478553_6", 32, 32, "b", nullptr, nullptr, "TXTR_RevBSmaller (options: Jump)"},
    TextureSpec{"tex1_32x32_10ca6dca25531ce9_4", 32, 32, "b", nullptr, nullptr, "B, RGB565 (GuiDVD/GuiNAND menus)"},
    // Z (Nunchuk)
    TextureSpec{"tex1_32x32_8a3e8ec9d97c84f7_6", 32, 32, "z", nullptr, nullptr, "TXTR_Z_Default (lock on)"},
    TextureSpec{"tex1_128x115_05ace719b52918bb_6", 128, 115, "z", nullptr, nullptr, "Nunchuk picture, Z prompt frame 1"},
    TextureSpec{"tex1_128x115_40ea01bf6f2aceae_6", 128, 115, "z", nullptr, nullptr, "Nunchuk picture, Z lit (frame 2)"},
    TextureSpec{"tex1_64x32_7f6a3b8dbc4a2cde_6", 64, 32, "z", nullptr, nullptr, "RevZButtonOn (options picture)"},
    // C (Nunchuk)
    TextureSpec{"tex1_32x32_52bc65cd8d4b0905_6", 32, 32, "c", nullptr, nullptr, "C (Morph Ball)"},
    TextureSpec{"tex1_128x115_7e4beb5a915f6b06_6", 128, 115, "c", nullptr, nullptr, "Nunchuk picture, C prompt frame 1"},
    TextureSpec{"tex1_128x115_223f7fb39403d23e_6", 128, 115, "c", nullptr, nullptr, "Nunchuk picture, C lit (frame 2)"},
    TextureSpec{"tex1_64x32_645dc2a0c6e63da4_6", 64, 32, "c", nullptr, nullptr, "RevCButtonOn (options picture)"},
    // - and +
    TextureSpec{"tex1_32x32_9af2d52683113ecc_6", 32, 32, "minus", nullptr, nullptr, "TXTR_Rev_Minus / TXTR_IconVisorSelectButton"},
    TextureSpec{"tex1_32x32_889e80824c922249_6", 32, 32, "minus", nullptr, nullptr, "TXTR_MinusSmaller (options: Visor)"},
    TextureSpec{"tex1_32x32_96342171475066ab_6", 32, 32, "plus", nullptr, nullptr, "TXTR_Rev_Plus / TXTR_IconHyperButton"},
    TextureSpec{"tex1_32x32_c3af34349deab463_6", 32, 32, "plus", nullptr, nullptr, "TXTR_PlusSmaller (options: Hyper)"},
    // 1, 2, HOME
    TextureSpec{"tex1_32x32_877ac67dc3c23f97_6", 32, 32, "one", nullptr, nullptr, "1 button"},
    TextureSpec{"tex1_32x32_b7bcef92f3f7378d_6", 32, 32, "one", nullptr, nullptr, "1 button pressed (animated frame)"},
    TextureSpec{"tex1_32x32_2d9b5d2d0a406a63_6", 32, 32, "two", nullptr, nullptr, "2 button (map help)"},
    TextureSpec{"tex1_32x32_b73270565c4c808d_5", 32, 32, "home", nullptr, nullptr, "StartButtonOut (plain circle, options: Exit)"},
    // D-pad
    TextureSpec{"tex1_32x32_e81f99c205eb24d0_6", 32, 32, nullptr, nullptr, "dpad", "TXTR_DPadDefault"},
    TextureSpec{"tex1_32x32_975e1a31b02a2a68_6", 32, 32, "dpad_down", nullptr, nullptr, "TXTR_DPadDown (seeker missiles)"},
    TextureSpec{"tex1_64x32_c52fa5e1704a2b4c_5", 64, 32, nullptr, nullptr, "dpad", "DPadN (options pictures)"},
    TextureSpec{"tex1_64x32_4d6e8ff9d2d981de_5", 64, 32, "dpad_up", nullptr, nullptr, "DPadU"},
    TextureSpec{"tex1_64x32_443f9c67227a5b2b_5", 64, 32, "dpad_down", nullptr, nullptr, "DPadD"},
    TextureSpec{"tex1_64x32_70185cb70118dc7e_5", 64, 32, "dpad_left", nullptr, nullptr, "DPadL"},
    TextureSpec{"tex1_64x32_ca3b3e2e642b4171_5", 64, 32, "dpad_right", nullptr, nullptr, "DPadR"},
    // Control stick
    TextureSpec{"tex1_40x32_4c3b4cc767632866_6", 40, 32, nullptr, nullptr, "stick", "LStickN (neutral)"},
    TextureSpec{"tex1_40x32_e65a03d74465035d_6", 40, 32, "stick_down", nullptr, nullptr, "LStickD"},
    TextureSpec{"tex1_40x32_bf0e66339a12f23a_6", 40, 32, "stick_right", nullptr, nullptr, "LStickR"},
    TextureSpec{"tex1_40x32_a60e7e0eac9a75e7_6", 40, 32, "stick_left", nullptr, nullptr, "LStickL"},
    TextureSpec{"tex1_40x32_970859e57fe24761_6", 40, 32, "stick_up", nullptr, nullptr, "TXTR_Thumbstick_Up"},
    TextureSpec{"tex1_64x32_54e47b41b25b4651_5", 64, 32, "stick_up", nullptr, nullptr, "LstickU (options pictures)"},
    TextureSpec{"tex1_40x32_c919255101f089a5_6", 40, 32, "stick_up", "stick_left", nullptr, "LStickUL"},
    TextureSpec{"tex1_40x32_fae180a397790a33_6", 40, 32, "stick_up", "stick_right", nullptr, "LStickUR"},
    TextureSpec{"tex1_40x32_ad10120cce78bfe8_6", 40, 32, "stick_down", "stick_left", nullptr, "LStickDL"},
    TextureSpec{"tex1_40x32_8824feb269d64f77_6", 40, 32, "stick_down", "stick_right", nullptr, "LStickDR"},
    // Motion prompts
    TextureSpec{"tex1_128x128_62ae16454d4083a4_6", 128, 128, "shake", nullptr, nullptr, "TXTR_Rev_ShakeBoth_Up"},
    TextureSpec{"tex1_128x128_f181b6e30ad22338_6", 128, 128, "shake", nullptr, nullptr, "TXTR_Rev_ShakeBoth_Down"},
    TextureSpec{"tex1_128x115_2431e9dab051193f_6", 128, 115, "push", "pull", nullptr, "Wii Remote tilt/push animation frame 1"},
    TextureSpec{"tex1_128x115_9f7fc8df609fe538_6", 128, 115, "push", "pull", nullptr, "Wii Remote tilt/push animation frame 2"},
    TextureSpec{"tex1_128x115_8f8f7f42a0fd97ec_6", 128, 115, "push", "pull", nullptr, "Wii Remote tilt/push animation frame 3"},
};

size_t ActionIndex(const char* id) {
    for (size_t i = 0; i < PrimeKeyBindings::ActionCount(); ++i)
        if (std::strcmp(PrimeKeyBindings::Action(i).id, id) == 0) return i;
    return SIZE_MAX;
}

unsigned PrimaryBinding(const char* id) {
    const size_t action = ActionIndex(id);
    if (action == SIZE_MAX) return 0;
    for (size_t slot = 0; slot < PrimeKeyBindings::kSlots; ++slot) {
        const unsigned code = PrimeKeyBindings::Binding(action, slot);
        if (code != 0) return code;
    }
    return 0;
}

#if defined(_WIN32)
// SDL scancodes are USB HID keyboard usages; Windows names keys by PS/2 scan
// code set 1, so map the usages we can show and ask GetKeyNameTextW, which
// answers in the OS language for the active keyboard layout (Ñ on a Spanish
// layout, "Leertaste" on German Windows, Q for the A position on AZERTY).
struct Set1 { uint16_t usage; uint16_t scan; bool extended; };
constexpr Set1 kSet1[] = {
    {0x04, 0x1E, false}, {0x05, 0x30, false}, {0x06, 0x2E, false}, {0x07, 0x20, false}, {0x08, 0x12, false},
    {0x09, 0x21, false}, {0x0A, 0x22, false}, {0x0B, 0x23, false}, {0x0C, 0x17, false}, {0x0D, 0x24, false},
    {0x0E, 0x25, false}, {0x0F, 0x26, false}, {0x10, 0x32, false}, {0x11, 0x31, false}, {0x12, 0x18, false},
    {0x13, 0x19, false}, {0x14, 0x10, false}, {0x15, 0x13, false}, {0x16, 0x1F, false}, {0x17, 0x14, false},
    {0x18, 0x16, false}, {0x19, 0x2F, false}, {0x1A, 0x11, false}, {0x1B, 0x2D, false}, {0x1C, 0x15, false},
    {0x1D, 0x2C, false},
    {0x1E, 0x02, false}, {0x1F, 0x03, false}, {0x20, 0x04, false}, {0x21, 0x05, false}, {0x22, 0x06, false},
    {0x23, 0x07, false}, {0x24, 0x08, false}, {0x25, 0x09, false}, {0x26, 0x0A, false}, {0x27, 0x0B, false},
    {0x2A, 0x0E, false}, {0x2B, 0x0F, false}, {0x2C, 0x39, false}, {0x2D, 0x0C, false}, {0x2E, 0x0D, false},
    {0x2F, 0x1A, false}, {0x30, 0x1B, false}, {0x31, 0x2B, false}, {0x32, 0x2B, false}, {0x33, 0x27, false},
    {0x34, 0x28, false}, {0x35, 0x29, false}, {0x36, 0x33, false}, {0x37, 0x34, false}, {0x38, 0x35, false},
    {0x39, 0x3A, false},
    {0x3A, 0x3B, false}, {0x3B, 0x3C, false}, {0x3C, 0x3D, false}, {0x3D, 0x3E, false}, {0x3E, 0x3F, false},
    {0x3F, 0x40, false}, {0x40, 0x41, false}, {0x41, 0x42, false}, {0x42, 0x43, false}, {0x43, 0x44, false},
    {0x44, 0x57, false}, {0x45, 0x58, false},
    {0x47, 0x46, false}, {0x49, 0x52, true}, {0x4A, 0x47, true}, {0x4B, 0x49, true}, {0x4C, 0x53, true},
    {0x4D, 0x4F, true}, {0x4E, 0x51, true}, {0x53, 0x45, false}, {0x54, 0x35, true}, {0x55, 0x37, false},
    {0x56, 0x4A, false}, {0x57, 0x4E, false}, {0x58, 0x1C, true}, {0x59, 0x4F, false}, {0x5A, 0x50, false},
    {0x5B, 0x51, false}, {0x5C, 0x4B, false}, {0x5D, 0x4C, false}, {0x5E, 0x4D, false}, {0x5F, 0x47, false},
    {0x60, 0x48, false}, {0x61, 0x49, false}, {0x62, 0x52, false}, {0x63, 0x53, false}, {0x64, 0x56, false},
};

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string WindowsKeyName(SDL_Scancode scancode) {
    for (const auto& k : kSet1) {
        if (k.usage != static_cast<uint16_t>(scancode)) continue;
        LONG lparam = static_cast<LONG>(k.scan) << 16;
        if (k.extended) lparam |= 1 << 24;
        wchar_t name[64]{};
        const int n = GetKeyNameTextW(lparam, name, 64);
        if (n <= 0) return {};
        std::wstring wide(name, static_cast<size_t>(n));
        // Windows shouts some names ("ESPACIO" on older layouts): keep the first capital only
        bool allUpper = wide.size() > 2;
        for (wchar_t c : wide) if (iswlower(c)) { allUpper = false; break; }
        if (allUpper) for (size_t i = 1; i < wide.size(); ++i) wide[i] = static_cast<wchar_t>(towlower(wide[i]));
        return Narrow(wide);
    }
    return {};
}

bool SpanishUi() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH; }
#else
std::string WindowsKeyName(SDL_Scancode) { return {}; }
bool SpanishUi() { return true; }
#endif
}  // namespace

const TextureSpec* Table(size_t* count) {
    if (count) *count = kTable.size();
    return kTable.data();
}

std::string KeyLabel(unsigned code) {
    if (code == 0) return "-";
    if (code & PrimeKeyBindings::kMouseBit) {
        static constexpr const char* kMouseEs[] = {"", "Clic izq.", "Clic med.", "Clic der.", "Raton 4", "Raton 5"};
        static constexpr const char* kMouseEn[] = {"", "L-Click", "M-Click", "R-Click", "Mouse 4", "Mouse 5"};
        const unsigned button = code & 0xFFu;
        const auto& table = SpanishUi() ? kMouseEs : kMouseEn;
        return button < std::size(table) ? table[button] : "Mouse";
    }
    const auto scancode = static_cast<SDL_Scancode>(code);
    // Universal short legends (as printed on most keyboards) for the keys whose
    // OS names are long or vary; everything else comes from Windows.
    struct Short { SDL_Scancode scancode; const char* label; };
    static constexpr Short kShort[] = {
        {SDL_SCANCODE_RETURN, "Enter"}, {SDL_SCANCODE_ESCAPE, "Esc"}, {SDL_SCANCODE_LSHIFT, "Shift"},
        {SDL_SCANCODE_RSHIFT, "Shift"}, {SDL_SCANCODE_LCTRL, "Ctrl"}, {SDL_SCANCODE_RCTRL, "Ctrl"},
        {SDL_SCANCODE_LALT, "Alt"}, {SDL_SCANCODE_RALT, "Alt Gr"}, {SDL_SCANCODE_TAB, "Tab"},
        {SDL_SCANCODE_LGUI, "Win"}, {SDL_SCANCODE_RGUI, "Win"},
        {SDL_SCANCODE_UP, "\xE2\x86\x91"}, {SDL_SCANCODE_DOWN, "\xE2\x86\x93"},
        {SDL_SCANCODE_LEFT, "\xE2\x86\x90"}, {SDL_SCANCODE_RIGHT, "\xE2\x86\x92"},
    };
    for (const auto& s : kShort)
        if (s.scancode == scancode) return s.label;
    if (std::string name = WindowsKeyName(scancode); !name.empty()) return name;
    std::string name = PrimeKeyBindings::BindingName(code);
    if (name.rfind("Keypad ", 0) == 0) name = "Num " + name.substr(7);
    return name;
}

#if defined(_WIN32)
namespace {
struct GdiplusSession {
    ULONG_PTR token = 0;
    bool ok = false;
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        ok = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }
    ~GdiplusSession() { if (ok) Gdiplus::GdiplusShutdown(token); }
};

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

bool PngEncoder(CLSID& clsid) {
    UINT count = 0, bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) return false;
    std::vector<unsigned char> buffer(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i)
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) { clsid = codecs[i].Clsid; return true; }
    return false;
}

// Draws `lines` centred inside a rounded key cap and saves the PNG flipped
// vertically: Retro's TXTR rows are stored bottom-up and the game's UVs expect
// that, so the file must match the original row order.
bool RenderKeyCap(const std::filesystem::path& path, unsigned width, unsigned height,
                  const std::vector<std::wstring>& lines, const CLSID& png) {
    using namespace Gdiplus;
    constexpr unsigned kScale = 4;
    const int W = static_cast<int>(width * kScale), H = static_cast<int>(height * kScale);
    Bitmap bitmap(W, H, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0, 0, 0, 0));
    // flip rows (see above)
    g.ScaleTransform(1.0f, -1.0f);
    g.TranslateTransform(0.0f, -static_cast<float>(H));

    // key cap: square-ish caps for square textures, a pill for wide ones
    const float margin = static_cast<float>(min(W, H)) * 0.06f;
    const RectF cap(margin, margin, W - 2 * margin, H - 2 * margin);
    const float radius = min(cap.Width, cap.Height) * 0.22f;
    GraphicsPath pathCap;
    pathCap.AddArc(cap.X, cap.Y, radius * 2, radius * 2, 180, 90);
    pathCap.AddArc(cap.X + cap.Width - radius * 2, cap.Y, radius * 2, radius * 2, 270, 90);
    pathCap.AddArc(cap.X + cap.Width - radius * 2, cap.Y + cap.Height - radius * 2, radius * 2, radius * 2, 0, 90);
    pathCap.AddArc(cap.X, cap.Y + cap.Height - radius * 2, radius * 2, radius * 2, 90, 90);
    pathCap.CloseFigure();
    SolidBrush fill(Color(235, 28, 32, 40));
    g.FillPath(&fill, &pathCap);
    Pen border(Color(255, 236, 240, 248), max(2.0f, static_cast<float>(min(W, H)) * 0.035f));
    g.DrawPath(&border, &pathCap);

    // text: the largest size whose longest line fits
    const wchar_t* faces[] = {L"Segoe UI", L"Arial"};
    const float inner = cap.Width * 0.84f;
    const float lineBudget = cap.Height * 0.84f / static_cast<float>(lines.size());
    float size = min(lineBudget * 0.8f, static_cast<float>(H) * 0.6f);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    SolidBrush ink(Color(255, 255, 255, 255));
    for (const wchar_t* face : faces) {
        FontFamily family(face);
        if (!family.IsAvailable()) continue;
        for (; size >= 6.0f; size *= 0.92f) {
            Font font(&family, size, FontStyleBold, UnitPixel);
            bool fits = true;
            for (const auto& line : lines) {
                RectF box;
                g.MeasureString(line.c_str(), -1, &font, PointF(0, 0), &format, &box);
                if (box.Width > inner || box.Height > lineBudget) { fits = false; break; }
            }
            if (!fits) continue;
            const float lineHeight = cap.Height * 0.84f / static_cast<float>(lines.size());
            const float top = cap.Y + (cap.Height - lineHeight * lines.size()) / 2;
            for (size_t i = 0; i < lines.size(); ++i) {
                RectF box(cap.X, top + lineHeight * static_cast<float>(i), cap.Width, lineHeight);
                g.DrawString(lines[i].c_str(), -1, &font, box, &format, &ink);
            }
            return bitmap.Save(path.wstring().c_str(), &png, nullptr) == Ok;
        }
        break;
    }
    return false;
}
}  // namespace
#endif

#if defined(_WIN32)
namespace {
// One gamepad symbol (prime_gamepad.h Glyph) inside `box`. Drawing coordinates
// are the picture as the game shows it (y down; RenderKeyCap's flip applies).
void DrawPadGlyph(Gdiplus::Graphics& g, const Gdiplus::RectF& box, const PrimeGamepad::Glyph& glyph) {
    using namespace Gdiplus;
    const float side = min(box.Width, box.Height);
    const float cx = box.X + box.Width / 2, cy = box.Y + box.Height / 2;
    const Color symbol((glyph.color >> 24) & 0xFF, (glyph.color >> 16) & 0xFF, (glyph.color >> 8) & 0xFF, glyph.color & 0xFF);
    SolidBrush fill(Color(235, 28, 32, 40));
    Pen border(Color(255, 236, 240, 248), max(2.0f, side * 0.05f));
    const auto text = [&](const std::wstring& value, const RectF& area, Color ink) {
        if (value.empty()) return;
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);
        SolidBrush brush(ink);
        for (const wchar_t* face : {L"Segoe UI", L"Arial"}) {
            FontFamily family(face);
            if (!family.IsAvailable()) continue;
            for (float size = area.Height * 0.62f; size >= 6.0f; size *= 0.92f) {
                Font font(&family, size, FontStyleBold, UnitPixel);
                RectF measured;
                g.MeasureString(value.c_str(), -1, &font, PointF(0, 0), &format, &measured);
                if (measured.Width > area.Width * 0.86f || measured.Height > area.Height * 0.9f) continue;
                g.DrawString(value.c_str(), -1, &font, area, &format, &brush);
                return;
            }
            return;
        }
    };
    // Screen offset of the glyph's direction (dirY up = smaller y).
    const float dirX = static_cast<float>(glyph.dirX), dirY = static_cast<float>(-glyph.dirY);
    const auto arrow = [&](float x, float y, float size, Color color) {
        const float length = std::sqrt(dirX * dirX + dirY * dirY);
        if (!(length > 0)) return;
        const float ux = dirX / length, uy = dirY / length, px = -uy, py = ux;
        const PointF points[] = {
            PointF(x + ux * size, y + uy * size),
            PointF(x - ux * size * 0.2f + px * size * 0.7f, y - uy * size * 0.2f + py * size * 0.7f),
            PointF(x - ux * size * 0.2f - px * size * 0.7f, y - uy * size * 0.2f - py * size * 0.7f)};
        SolidBrush brush(color);
        g.FillPolygon(&brush, points, 3);
    };
    const RectF disc(cx - side * 0.44f, cy - side * 0.44f, side * 0.88f, side * 0.88f);
    switch (glyph.shape) {
    case PrimeGamepad::Shape::Round:
    case PrimeGamepad::Shape::Cross:
    case PrimeGamepad::Shape::Circle:
    case PrimeGamepad::Shape::Square:
    case PrimeGamepad::Shape::Triangle: {
        g.FillEllipse(&fill, disc);
        g.DrawEllipse(&border, disc);
        Pen mark(symbol, side * 0.09f);
        const float r = side * 0.22f;
        if (glyph.shape == PrimeGamepad::Shape::Cross) {
            g.DrawLine(&mark, cx - r, cy - r, cx + r, cy + r);
            g.DrawLine(&mark, cx - r, cy + r, cx + r, cy - r);
        } else if (glyph.shape == PrimeGamepad::Shape::Circle) {
            g.DrawEllipse(&mark, cx - r, cy - r, 2 * r, 2 * r);
        } else if (glyph.shape == PrimeGamepad::Shape::Square) {
            g.DrawRectangle(&mark, cx - r * 0.9f, cy - r * 0.9f, 1.8f * r, 1.8f * r);
        } else if (glyph.shape == PrimeGamepad::Shape::Triangle) {
            const PointF points[] = {PointF(cx, cy - r * 1.05f), PointF(cx - r * 1.1f, cy + r * 0.75f), PointF(cx + r * 1.1f, cy + r * 0.75f)};
            g.DrawPolygon(&mark, points, 3);
        } else {
            text(Widen(glyph.text), disc, symbol);
        }
        break;
    }
    case PrimeGamepad::Shape::Shoulder: {
        const float h = min(box.Height * 0.62f, box.Width * 0.7f), w = min(box.Width * 0.92f, h * 2.2f);
        const RectF bar(cx - w / 2, cy - h / 2, w, h);
        const float radius = h * 0.35f;
        GraphicsPath path;
        path.AddArc(bar.X, bar.Y, radius * 2, radius * 2, 180, 90);
        path.AddArc(bar.X + bar.Width - radius * 2, bar.Y, radius * 2, radius * 2, 270, 90);
        path.AddArc(bar.X + bar.Width - radius * 2, bar.Y + bar.Height - radius * 2, radius * 2, radius * 2, 0, 90);
        path.AddArc(bar.X, bar.Y + bar.Height - radius * 2, radius * 2, radius * 2, 90, 90);
        path.CloseFigure();
        g.FillPath(&fill, &path);
        g.DrawPath(&border, &path);
        text(Widen(glyph.text), bar, Color(255, 255, 255, 255));
        break;
    }
    case PrimeGamepad::Shape::Dpad: {
        const float arm = side * 0.28f, half = side * 0.42f;
        SolidBrush body(Color(240, 70, 74, 84));
        g.FillRectangle(&body, cx - arm / 2, cy - half, arm, 2 * half);
        g.FillRectangle(&body, cx - half, cy - arm / 2, 2 * half, arm);
        if (glyph.dirX != 0 || glyph.dirY != 0) {
            SolidBrush lit(Color(255, 245, 245, 250));
            const float ax = cx + dirX * half * 0.55f, ay = cy + dirY * half * 0.55f;
            g.FillRectangle(&lit, ax - arm / 2, ay - arm / 2, arm, arm);
        }
        break;
    }
    case PrimeGamepad::Shape::Stick: {
        g.FillEllipse(&fill, disc);
        g.DrawEllipse(&border, disc);
        Pen ring(Color(255, 150, 156, 170), side * 0.04f);
        g.DrawEllipse(&ring, cx - side * 0.3f, cy - side * 0.3f, side * 0.6f, side * 0.6f);
        arrow(cx + dirX * side * 0.3f, cy + dirY * side * 0.3f, side * 0.14f, Color(255, 245, 245, 250));
        text(Widen(glyph.text), RectF(cx - side * 0.3f, cy - side * 0.3f, side * 0.6f, side * 0.6f), Color(255, 255, 255, 255));
        break;
    }
    }
}

// One or two pad symbols: side by side on wide textures, overlapping
// diagonally on square ones; transparent elsewhere, rows flipped like RenderKeyCap.
bool RenderPadIcon(const std::filesystem::path& path, unsigned width, unsigned height,
                   const std::vector<PrimeGamepad::Glyph>& glyphs, const CLSID& png) {
    using namespace Gdiplus;
    constexpr unsigned kScale = 4;
    const int W = static_cast<int>(width * kScale), H = static_cast<int>(height * kScale);
    Bitmap bitmap(W, H, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0, 0, 0, 0));
    g.ScaleTransform(1.0f, -1.0f);
    g.TranslateTransform(0.0f, -static_cast<float>(H));
    const float w = static_cast<float>(W), h = static_cast<float>(H);
    if (glyphs.size() == 1) {
        DrawPadGlyph(g, RectF(0, 0, w, h), glyphs[0]);
    } else if (w >= h * 1.5f) {
        DrawPadGlyph(g, RectF(0, 0, w / 2, h), glyphs[0]);
        DrawPadGlyph(g, RectF(w / 2, 0, w / 2, h), glyphs[1]);
    } else {
        DrawPadGlyph(g, RectF(0, 0, w * 0.62f, h * 0.62f), glyphs[0]);
        DrawPadGlyph(g, RectF(w * 0.38f, h * 0.38f, w * 0.62f, h * 0.62f), glyphs[1]);
    }
    return bitmap.Save(path.wstring().c_str(), &png, nullptr) == Ok;
}
}  // namespace
#endif

const char* PadVariant(PrimeGamepad::Style style) {
    switch (style) {
    case PrimeGamepad::Style::Xbox: return "pad_xbox";
    case PrimeGamepad::Style::PlayStation: return "pad_playstation";
    case PrimeGamepad::Style::Nintendo: return "pad_nintendo";
    case PrimeGamepad::Style::GameCube: return "pad_gamecube";
    default: return "pad_generic";
    }
}

size_t GeneratePadIcons(const std::filesystem::path& userDataDirectory, PrimeGamepad::Style style) {
#if defined(_WIN32)
    GdiplusSession session;
    CLSID png{};
    if (!session.ok || !PngEncoder(png)) return 0;
    using PrimeGamepad::Input;
    // The pad input shown for a Wii action: its primary binding, else the secondary.
    const auto bound = [](const char* action) {
        for (size_t i = 0; i < PrimePadBindings::ActionCount(); ++i) {
            if (std::strcmp(PrimePadBindings::ActionId(i), action) != 0) continue;
            for (size_t slot = 0; slot < PrimePadBindings::kSlots; ++slot)
                if (const Input input = PrimePadBindings::Binding(i, slot); input != Input::None) return input;
        }
        return Input::None;
    };
    const auto layerBound = [](const char* action) {
        for (size_t i = 0; i < PrimePadBindings::ActionCount(); ++i)
            if (std::strcmp(PrimePadBindings::ActionId(i), action) == 0)
                return PrimePadBindings::Binding(i, PrimePadBindings::kLayerSlot);
        return Input::None;
    };
    // On a pad the Nunchuk stick is the left stick.
    const auto stickInput = [](const char* action) {
        if (std::strcmp(action, "stick_up") == 0) return Input::LeftStickUp;
        if (std::strcmp(action, "stick_down") == 0) return Input::LeftStickDown;
        if (std::strcmp(action, "stick_left") == 0) return Input::LeftStickLeft;
        if (std::strcmp(action, "stick_right") == 0) return Input::LeftStickRight;
        return Input::None;
    };
    size_t written = 0;
    {
        const auto folder = userDataDirectory / "texture_replacements" / "variants" / PadVariant(style);
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec) return 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            if (entry.is_regular_file(ec) && entry.path().extension() == ".png") std::filesystem::remove(entry.path(), ec);
        const auto glyph = [&](Input input) { return PrimeGamepad::GlyphFor(style, input); };
        for (const auto& spec : kTable) {
            std::vector<PrimeGamepad::Glyph> glyphs;
            if (spec.actionA && stickInput(spec.actionA) != Input::None) {
                // Diagonals: one stick symbol with the combined arrow.
                auto g = glyph(stickInput(spec.actionA));
                if (spec.actionB && stickInput(spec.actionB) != Input::None) {
                    const auto other = glyph(stickInput(spec.actionB));
                    g.dirX = static_cast<int8_t>(g.dirX + other.dirX);
                    g.dirY = static_cast<int8_t>(g.dirY + other.dirY);
                }
                glyphs.push_back(g);
            } else if (spec.actionA && (std::strcmp(spec.actionA, "push") == 0 || std::strcmp(spec.actionA, "pull") == 0)) {
                // Push/pull: hold the motion mode button and move the right stick.
                if (const Input motion = bound(PrimeGamepad::kMotionModeAction); motion != Input::None) glyphs.push_back(glyph(motion));
                glyphs.push_back(PrimeGamepad::Glyph{PrimeGamepad::Shape::Stick, "R", 0xFFE8E8E8u, 0, 1});
            } else if (spec.actionA) {
                const auto add = [&](const char* action) {
                    if (const Input input = bound(action); input != Input::None) {
                        glyphs.push_back(glyph(input));
                        return;
                    }
                    // Only a motion-layer combination: the motion button, then the button.
                    if (const Input input = layerBound(action); input != Input::None) {
                        if (const Input motion = bound(PrimeGamepad::kMotionModeAction); motion != Input::None)
                            glyphs.push_back(glyph(motion));
                        glyphs.push_back(glyph(input));
                    }
                };
                add(spec.actionA);
                if (spec.actionB) add(spec.actionB);
            } else if (spec.fixed && std::strcmp(spec.fixed, "stick") == 0) {
                glyphs.push_back(PrimeGamepad::Glyph{PrimeGamepad::Shape::Stick, "L", 0xFFE8E8E8u});
            } else if (spec.fixed && std::strcmp(spec.fixed, "dpad") == 0) {
                glyphs.push_back(PrimeGamepad::Glyph{PrimeGamepad::Shape::Dpad, ""});
            }
            if (glyphs.empty()) continue;  // unbound: the game's own Wii picture stays
            if (glyphs.size() > 2) glyphs.resize(2);
            if (RenderPadIcon(folder / (std::string(spec.file) + ".png"), spec.width, spec.height, glyphs, png)) ++written;
        }
    }
    RT_LOG(RT_TAG_RUNTIME) << "[prime-pad-icons] " << written << " icons written for " << PadVariant(style) << std::endl;
    return written;
#else
    (void)userDataDirectory;
    (void)style;
    return 0;
#endif
}

size_t Generate(const std::filesystem::path& userDataDirectory) {
#if defined(_WIN32)
    GdiplusSession session;
    CLSID png{};
    if (!session.ok || !PngEncoder(png)) {
        RT_LOG(RT_TAG_RUNTIME) << "[prime-key-icons] GDI+ unavailable; icons not generated" << std::endl;
        return 0;
    }
    const auto folder = userDataDirectory / "texture_replacements" / "keyboard_icons";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        RT_LOG(RT_TAG_RUNTIME) << "[prime-key-icons] cannot create " << folder.string() << ": " << ec.message() << std::endl;
        return 0;
    }
    // Only our own files live here; drop stale ones (a remapped key changes the text, not the name).
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".png") std::filesystem::remove(entry.path(), ec);

    const auto label = [](const char* action) { return Widen(KeyLabel(PrimaryBinding(action))); };
    size_t written = 0;
    for (const auto& spec : kTable) {
        std::vector<std::wstring> lines;
        if (spec.actionA && spec.actionB) {
            lines = {label(spec.actionA) + L" / " + label(spec.actionB)};
            // long pairs on two lines
            if (lines[0].size() > 9) lines = {label(spec.actionA), label(spec.actionB)};
        } else if (spec.actionA) {
            lines = {label(spec.actionA)};
        } else if (spec.fixed && std::strcmp(spec.fixed, "stick") == 0) {
            lines = {label("stick_up"), label("stick_left") + L" " + label("stick_down") + L" " + label("stick_right")};
        } else if (spec.fixed && std::strcmp(spec.fixed, "dpad") == 0) {
            lines = {label("dpad_up") + L" " + label("dpad_down"), label("dpad_left") + L" " + label("dpad_right")};
        } else if (spec.fixed) {
            lines = {Widen(spec.fixed)};
        }
        if (lines.empty()) continue;
        const auto path = folder / (std::string(spec.file) + ".png");
        if (RenderKeyCap(path, spec.width, spec.height, lines, png)) ++written;
        else RT_LOG(RT_TAG_RUNTIME) << "[prime-key-icons] failed to render " << spec.file << std::endl;
    }
    RT_LOG(RT_TAG_RUNTIME) << "[prime-key-icons] " << written << "/" << kTable.size() << " icons written to "
                           << folder.string() << std::endl;
    return written;
#else
    (void)userDataDirectory;
    return 0;
#endif
}

}  // namespace PrimeKeyIcons
