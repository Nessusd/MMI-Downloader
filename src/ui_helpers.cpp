#include "ui_helpers.h"

#include "core.h"

#include <commctrl.h>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <shobjidl.h>
#include <string>
#include <wrl/client.h>

namespace fs = std::filesystem;

namespace mmid::ui {

std::wstring get_window_text(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<size_t>(length));
    return text;
}

void apply_font(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND make_control(HWND parent,
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    DWORD ex_style,
    int id) {
    DWORD window_style = WS_CHILD | WS_VISIBLE | style;
    if (wcscmp(class_name, L"STATIC") != 0 && wcscmp(class_name, PROGRESS_CLASSW) != 0) {
        window_style |= WS_TABSTOP;
    }
    HWND hwnd = CreateWindowExW(ex_style,
        class_name,
        text,
        window_style,
        0,
        0,
        10,
        10,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr);
    if (!hwnd) {
        throw AppError("CreateWindowEx failed");
    }
    return hwnd;
}

LRESULT handle_static_control_color(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter) {
    wchar_t class_name[16]{};
    GetClassNameW(reinterpret_cast<HWND>(long_parameter), class_name, 16);
    if (std::wcscmp(class_name, L"Static") != 0 &&
        std::wcscmp(class_name, L"Button") != 0) {
        return DefWindowProcW(window, message, word_parameter, long_parameter);
    }

    HDC device_context = reinterpret_cast<HDC>(word_parameter);
    SetBkMode(device_context, TRANSPARENT);
    return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
}


namespace {

FolderPickerResult folder_picker_failure(FolderPickerFailureStage stage, HRESULT code) {
    FolderPickerResult result;
    result.failure_stage = stage;
    result.failure_code = code;
    return result;
}

const char* folder_picker_failure_operation(FolderPickerFailureStage stage) {
    switch (stage) {
    case FolderPickerFailureStage::CreateDialog:
        return "CoCreateInstance(CLSID_FileOpenDialog)";
    case FolderPickerFailureStage::ShowDialog:
        return "IFileOpenDialog::Show";
    case FolderPickerFailureStage::GetResult:
        return "IFileOpenDialog::GetResult";
    case FolderPickerFailureStage::GetDisplayName:
        return "IShellItem::GetDisplayName";
    case FolderPickerFailureStage::None:
        return "Folder picker";
    }
    return "Folder picker";
}

} // namespace

[[noreturn]] void throw_folder_picker_failure(const FolderPickerResult& result) {
    throw AppError(std::string(folder_picker_failure_operation(result.failure_stage)) +
        " failed: HRESULT " +
        std::to_string(static_cast<unsigned long>(result.failure_code)));
}

FolderPickerResult pick_folder_path(
    HWND owner,
    const wchar_t* title,
    const std::wstring& initial_path) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(hr)) {
        return folder_picker_failure(FolderPickerFailureStage::CreateDialog, hr);
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    }
    dialog->SetTitle(title);

    if (!initial_path.empty() && fs::exists(initial_path)) {
        Microsoft::WRL::ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial_path.c_str(),
                nullptr,
                IID_PPV_ARGS(folder.GetAddressOf())))) {
            dialog->SetFolder(folder.Get());
        }
    }

    hr = dialog->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {};
    }
    if (FAILED(hr)) {
        return folder_picker_failure(FolderPickerFailureStage::ShowDialog, hr);
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    hr = dialog->GetResult(item.GetAddressOf());
    if (FAILED(hr)) {
        return folder_picker_failure(FolderPickerFailureStage::GetResult, hr);
    }

    PWSTR raw_value = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw_value);
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> raw_path(raw_value, &CoTaskMemFree);
    if (FAILED(hr)) {
        return folder_picker_failure(FolderPickerFailureStage::GetDisplayName, hr);
    }

    FolderPickerResult result;
    result.path = fs::path(raw_path.get());
    return result;
}


} // namespace mmid::ui
