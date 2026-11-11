#include "hle/prime_disc_picker.h"

#include "hle/prime_disc_path.h"
#include "prime_port_version.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#else
#include <SDL3/SDL.h>
#include <atomic>
#include <mutex>
#endif

namespace PrimeDiscPicker {

#if defined(_WIN32)
namespace {

constexpr wchar_t kDialogTitle[] =
    L"Select your Metroid Prime 3: Corruption (USA) or Metroid Prime Trilogy (USA) disc image";

enum class DialogOutcome { Chosen, Cancelled, Unavailable };

// The Vista file dialog, asked for a file system item and its file system path
// (SIGDN_FILESYSPATH). The old GetOpenFileNameW dialog returned a shell display
// name ("::{CLSID}\...") under Proton/Wine when the file was reached through
// This PC (discussion #4).
DialogOutcome ShowFileDialog(std::filesystem::path& chosen) {
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = SUCCEEDED(init);
    DialogOutcome outcome = DialogOutcome::Unavailable;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        const COMDLG_FILTERSPEC filters[] = {
            {L"Wii disc images (*.iso;*.rvz;*.wbfs;*.gcz;*.wia;*.ciso)", L"*.iso;*.rvz;*.wbfs;*.gcz;*.wia;*.ciso"},
            {L"All files (*.*)", L"*.*"},
        };
        dialog->SetFileTypes(2, filters);
        dialog->SetTitle(kDialogTitle);
        const HRESULT shown = dialog->Show(nullptr);
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            outcome = DialogOutcome::Cancelled;
        } else if (SUCCEEDED(shown)) {
            outcome = DialogOutcome::Cancelled;
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    chosen = std::filesystem::path(path);
                    outcome = DialogOutcome::Chosen;
                    ::CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (uninitialize) ::CoUninitialize();
    return outcome;
}

// Fallback when the COM dialog cannot be created.
DialogOutcome ShowLegacyDialog(std::filesystem::path& chosen) {
    wchar_t file[MAX_PATH * 4]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrTitle = kDialogTitle;
    dialog.lpstrFilter = L"Wii disc images (*.iso;*.rvz;*.wbfs;*.gcz;*.wia;*.ciso)\0*.iso;*.rvz;*.wbfs;*.gcz;*.wia;*.ciso\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!::GetOpenFileNameW(&dialog) || file[0] == L'\0') return DialogOutcome::Cancelled;
    chosen = std::filesystem::path(file);
    return DialogOutcome::Chosen;
}

std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

// UTF-8 text in a task-modal box (paths may hold any character).
int Message(const std::string& text, UINT flags) {
    const std::wstring title = Wide(std::string(PRIME_PORT_NAME) + " - Disc image");
    return ::MessageBoxW(nullptr, Wide(text).c_str(), title.c_str(), flags | MB_SETFOREGROUND | MB_TASKMODAL);
}

} // namespace
#else
namespace {

// SDL3's file dialog: the desktop portal (GNOME, KDE, ...) or zenity/kdialog.
// It is asynchronous, so the caller pumps events until the callback ran.
struct DialogState {
    std::atomic<bool> done{false};
    std::mutex mutex;
    std::filesystem::path chosen;
    bool failed = false;
};

void SDLCALL OnFileChosen(void* user, const char* const* files, int) {
    auto& state = *static_cast<DialogState*>(user);
    {
        std::lock_guard lock(state.mutex);
        if (files == nullptr) state.failed = true;  // the dialog could not be shown
        else if (files[0] != nullptr) state.chosen = std::filesystem::path(files[0]);
    }
    state.done.store(true);
}

bool ShowFileDialog(std::filesystem::path& chosen) {
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        RT_LOG(RT_TAG_DVD) << "[prime-iso-pick] no video subsystem for the dialog: " << SDL_GetError() << std::endl;
        return false;
    }
    static const SDL_DialogFileFilter filters[] = {
        {"Wii disc images (*.iso, *.rvz, *.wbfs, *.gcz, *.wia, *.ciso)", "iso;rvz;wbfs;gcz;wia;ciso"},
        {"All files", "*"},
    };
    DialogState state;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(filters));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                          "Select your Metroid Prime 3: Corruption (USA) or Metroid Prime Trilogy (USA) disc image");
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, OnFileChosen, &state, props);
    while (!state.done.load()) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    SDL_DestroyProperties(props);
    std::lock_guard lock(state.mutex);
    if (state.failed) RT_LOG(RT_TAG_DVD) << "[prime-iso-pick] file dialog unavailable: " << SDL_GetError() << std::endl;
    chosen = state.chosen;
    return !chosen.empty();
}

// UTF-8 text with OK/Cancel; true when OK was chosen.
bool Confirm(const std::string& text, bool warning) {
    const std::string title = std::string(PRIME_PORT_NAME) + " - Disc image";
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "OK"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
    };
    const SDL_MessageBoxData box{warning ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION, nullptr,
                                 title.c_str(), text.c_str(), 2, buttons, nullptr};
    int pressed = 0;
    if (!SDL_ShowMessageBox(&box, &pressed)) {
        RT_LOG(RT_TAG_DVD) << "[prime-iso-pick] message box unavailable: " << SDL_GetError() << std::endl;
        return true;  // still offer the file dialog
    }
    return pressed == 1;
}

} // namespace
#endif

std::filesystem::path Prompt(std::string_view reason) {
#if defined(_WIN32)
    if (!reason.empty()) {
        std::string text(reason);
        text += "\n\nClick OK to choose the disc image (.iso, .rvz, .wbfs, .gcz, .wia or .ciso). "
                "The path is saved in UserData/Config.toml once the image opens, so this is only asked once.";
        if (Message(text, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
            return {};
        }
    }
    for (;;) {
        std::filesystem::path chosen;
        DialogOutcome outcome = ShowFileDialog(chosen);
        if (outcome == DialogOutcome::Unavailable) {
            RT_LOG(RT_TAG_DVD) << "[prime-iso-pick] the Vista file dialog is unavailable; using the legacy dialog" << std::endl;
            outcome = ShowLegacyDialog(chosen);
        }
        if (outcome != DialogOutcome::Chosen) return {};
        const std::string utf8 = RuntimeConfigFile::PathToUtf8(chosen);
        const PrimeDiscPath::Problem problem = PrimeDiscPath::Check(chosen, utf8);
        if (problem == PrimeDiscPath::Problem::None && chosen.is_absolute()) return chosen;
        RT_LOG(RT_TAG_DVD) << "[prime-iso-pick] refused dialog result=" << utf8 << std::endl;
        const std::string text = "The dialog returned a location that is not a disc image file:\n  " + utf8 + "\n\n" +
                                 PrimeDiscPath::ProblemText(problem == PrimeDiscPath::Problem::None ? PrimeDiscPath::Problem::ShellLocation : problem) +
                                 "\n\nClick OK to choose again.";
        if (Message(text, MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            return {};
        }
    }
#else
    if (!reason.empty()) {
        std::string text(reason);
        text += "\n\nClick OK to choose the disc image (.iso, .rvz, .wbfs, .gcz, .wia or .ciso). "
                "The path is saved in UserData/Config.toml once the image opens, so this is only asked once.";
        if (!Confirm(text, false)) return {};
    }
    for (;;) {
        std::filesystem::path chosen;
        if (!ShowFileDialog(chosen)) return {};
        const std::string utf8 = RuntimeConfigFile::PathToUtf8(chosen);
        const PrimeDiscPath::Problem problem = PrimeDiscPath::Check(chosen, utf8);
        if (problem == PrimeDiscPath::Problem::None && chosen.is_absolute()) return chosen;
        RT_LOG(RT_TAG_DVD) << "[prime-iso-pick] refused dialog result=" << utf8 << std::endl;
        const std::string text = "The dialog returned a location that is not a disc image file:\n  " + utf8 + "\n\n" +
                                 PrimeDiscPath::ProblemText(problem == PrimeDiscPath::Problem::None ? PrimeDiscPath::Problem::ShellLocation : problem) +
                                 "\n\nClick OK to choose again.";
        if (!Confirm(text, true)) return {};
    }
#endif
}

} // namespace PrimeDiscPicker
