#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace mmid::ui {

std::wstring get_window_text(HWND window);
void apply_font(HWND window, HFONT font);
HWND make_control(
    HWND parent,
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    DWORD extended_style,
    int id);
LRESULT handle_static_control_color(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter);

enum class FolderPickerFailureStage {
    None,
    CreateDialog,
    ShowDialog,
    GetResult,
    GetDisplayName
};

struct FolderPickerResult {
    std::optional<std::filesystem::path> path;
    FolderPickerFailureStage failure_stage = FolderPickerFailureStage::None;
    HRESULT failure_code = S_OK;
};

FolderPickerResult pick_folder_path(
    HWND owner,
    const wchar_t* title,
    const std::wstring& initial_path);
[[noreturn]] void throw_folder_picker_failure(const FolderPickerResult& result);

} // namespace mmid::ui
