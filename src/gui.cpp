#include "core.h"
#include "package_preparation.h"
#include "removable_drive.h"
#include "settings_dialog.h"
#include "ui_helpers.h"

#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <memory>
#include <objbase.h>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace mmid;
using ui::FolderPickerFailureStage;
using ui::FolderPickerResult;
using ui::apply_font;
using ui::get_window_text;
using ui::handle_static_control_color;
using ui::make_control;
using ui::pick_folder_path;
using ui::show_settings_dialog;
using ui::throw_folder_picker_failure;

constexpr wchar_t kWindowClass[] = L"MMIDownloaderWindow";
constexpr int kAppIconResourceId = 101;
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_ERROR = WM_APP + 2;
constexpr UINT WM_APP_BRANDS = WM_APP + 3;
constexpr UINT WM_APP_RESOLVE_DONE = WM_APP + 4;
constexpr UINT WM_APP_PREPARE_DONE = WM_APP + 5;
constexpr UINT WM_APP_PROGRESS = WM_APP + 6;
constexpr UINT WM_APP_MODELS = WM_APP + 7;
constexpr UINT WM_APP_WORKER_FINISHED = WM_APP + 8;
constexpr UINT_PTR kWorkerReapTimerId = 5002;
constexpr UINT kWorkerReapTimerMs = 20;

enum class WorkerKind {
    None,
    Brands,
    Models,
    Resolve,
    Prepare
};

enum ControlId {
    IDC_BRAND = 1001,
    IDC_PART = 1002,
    IDC_DEST = 1003,
    IDC_BROWSE = 1004,
    IDC_REFRESH = 1005,
    IDC_RESOLVE = 1006,
    IDC_START = 1007,
    IDC_INCLUDE_HIDDEN = 1008,
    IDC_LOG = 1009,
    IDC_PROGRESS = 1011,
    IDC_STATUS = 1012,
    IDC_DRIVE = 1013,
    IDC_REFRESH_DRIVES = 1014,
    IDC_MODEL = 1015,
    IDC_EXTRACT_TO_FOLDER = 1016,
    IDC_PACKAGE_DETAILS = 1017,
    IDC_CANCEL = 1018,
    IDC_SD_CARD_MODE = 1020
};

enum MenuId {
    IDM_SETTINGS = 4001,
    IDM_ABOUT = 4002
};

struct ResolveResult {
    uint64_t request_id = 0;
    Brand brand;
    PartResolution resolution;
    PackageInfo package;
    fs::path package_xml;
};

struct PreparePayload {
    uint64_t request_id = 0;
    PreparationOutcome outcome;
};

struct ProgressUpdate {
    uint64_t request_id = 0;
    WorkerKind kind = WorkerKind::None;
    int value = 0;
    std::wstring text;
};

struct WorkerTextPayload {
    uint64_t request_id = 0;
    WorkerKind kind = WorkerKind::None;
    std::wstring text;
};

struct BrandsPayload {
    uint64_t request_id = 0;
    std::vector<Brand> brands;
};

struct ModelsPayload {
    uint64_t request_id = 0;
    std::wstring brand_id;
    std::wstring brand_alias;
    std::vector<ModelInfo> models;
    std::vector<PartInfo> parts;
};

struct OwnedWorker {
    WorkerKind kind = WorkerKind::None;
    std::shared_ptr<std::atomic_bool> cancel_requested;
    std::thread thread;
    bool finished_notified = false;
};

struct GuiState {
    HWND window = nullptr;
    HWND brand_combo = nullptr;
    HWND model_combo = nullptr;
    HWND part_edit = nullptr;
    HWND dest_edit = nullptr;
    HWND browse_button = nullptr;
    HWND refresh_button = nullptr;
    HWND resolve_button = nullptr;
    HWND start_button = nullptr;
    HWND cancel_button = nullptr;
    HWND include_hidden = nullptr;
    HWND extract_to_folder = nullptr;
    HWND sd_card_mode = nullptr;
    HWND drive_combo = nullptr;
    HWND refresh_drives_button = nullptr;
    HWND log_edit = nullptr;
    HWND package_details_edit = nullptr;
    HWND progress = nullptr;
    HWND status_static = nullptr;
    HFONT font = nullptr;
    bool busy = false;
    std::wstring status_text = L"Ready";
    std::vector<Brand> brands;
    std::vector<ModelInfo> models;
    std::vector<PartInfo> parts;
    std::vector<RemovableDriveInfo> drives;
    std::optional<ResolveResult> last_resolution;
    std::shared_ptr<std::atomic_bool> cancel_requested;
    std::unordered_map<uint64_t, std::unique_ptr<OwnedWorker>> workers;
    uint64_t next_request_id = 0;
    uint64_t active_request_id = 0;
    uint64_t active_model_request_id = 0;
    bool close_requested = false;
    bool suppress_brand_change = false;
};

std::wstring clean_part_number(std::wstring value) {
    value = trim_w(std::move(value));
    value.erase(std::remove(value.begin(), value.end(), L'.'), value.end());
    return value;
}

std::wstring bytes_to_gb_text(uint64_t bytes) {
    double gb = static_cast<double>(bytes) / 1024.0 / 1024.0 / 1024.0;
    std::wostringstream out;
    out << std::fixed << std::setprecision(gb < 10.0 ? 1 : 0) << gb << L" GB";
    return out.str();
}

std::wstring text_or_dash(const std::wstring& value) {
    std::wstring trimmed = trim_w(value);
    return trimmed.empty() ? L"-" : trimmed;
}

std::wstring byte_count_text(uint64_t bytes) {
    if (bytes == 0) {
        return L"0 bytes";
    }

    double size = static_cast<double>(bytes);
    const wchar_t* unit = L"B";
    if (size >= 1024.0 * 1024.0 * 1024.0) {
        size /= 1024.0 * 1024.0 * 1024.0;
        unit = L"GB";
    } else if (size >= 1024.0 * 1024.0) {
        size /= 1024.0 * 1024.0;
        unit = L"MB";
    } else if (size >= 1024.0) {
        size /= 1024.0;
        unit = L"KB";
    }

    int precision = (size < 10.0 && std::wcscmp(unit, L"B") != 0) ? 1 : 0;
    std::wostringstream out;
    out << std::fixed << std::setprecision(precision)
        << size << L" " << unit << L" (" << bytes << L" bytes)";
    return out.str();
}

std::wstring drive_display_text(const RemovableDriveInfo& drive) {
    std::wstring text = drive.root;
    if (!drive.label.empty()) {
        text += L" ";
        text += drive.label;
    }
    text += L" (";
    text += bytes_to_gb_text(drive.total_bytes);
    text += L")";
    return text;
}

std::wstring chain_text(const std::vector<std::wstring>& chain) {
    std::wstring result;
    for (size_t i = 0; i < chain.size(); ++i) {
        if (i != 0) {
            result += L" -> ";
        }
        result += chain[i];
    }
    return result;
}

void post_worker_text(HWND window,
    UINT message,
    uint64_t request_id,
    WorkerKind kind,
    std::wstring text) {
    auto* payload = new WorkerTextPayload;
    payload->request_id = request_id;
    payload->kind = kind;
    payload->text = std::move(text);
    if (!PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

void post_log(HWND window, uint64_t request_id, WorkerKind kind, std::wstring text) {
    post_worker_text(window, WM_APP_LOG, request_id, kind, std::move(text));
}

void post_error(HWND window, uint64_t request_id, WorkerKind kind, std::wstring text) {
    post_worker_text(window, WM_APP_ERROR, request_id, kind, std::move(text));
}

void post_error(HWND window, std::wstring text) {
    post_error(window, 0, WorkerKind::None, std::move(text));
}

void post_progress(HWND window, uint64_t request_id, WorkerKind kind, int value, std::wstring text) {
    auto* update = new ProgressUpdate;
    update->request_id = request_id;
    update->kind = kind;
    update->value = value;
    update->text = std::move(text);
    if (!PostMessageW(window, WM_APP_PROGRESS, 0, reinterpret_cast<LPARAM>(update))) {
        delete update;
    }
}

void show_about_dialog(HWND window) {
    std::wstring text =
        L"MMI Downloader\r\n"
        L"Version: " + utf8_to_wide(app_version()) + L"\r\n"
        L"Build: " + utf8_to_wide(build_number()) + L"\r\n"
        L"User-Agent: " + app_user_agent() + L"\r\n\r\n"
        L"Contribution and copyright: Nessus\r\n"
        L"Contacts: @Nessus\r\n"
        L"Email: neo@sknt.su";
    MessageBoxW(window, text.c_str(), L"About", MB_ICONINFORMATION);
}

template <typename T>
T* take_payload(LPARAM lparam) {
    return reinterpret_cast<T*>(lparam);
}

GuiState* state_from(HWND window) {
    return reinterpret_cast<GuiState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void append_log(GuiState& state, const std::wstring& text) {
    int length = GetWindowTextLengthW(state.log_edit);
    SendMessageW(state.log_edit, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t timestamp[16]{};
    swprintf_s(timestamp, L"[%02u:%02u:%02u] ", now.wHour, now.wMinute, now.wSecond);
    std::wstring line = timestamp;
    line += text;
    if (line.empty() || line.back() != L'\n') {
        line += L"\r\n";
    }
    SendMessageW(state.log_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void clear_log(GuiState& state) {
    SetWindowTextW(state.log_edit, L"");
}

void set_status(GuiState& state, const std::wstring& text) {
    state.status_text = text;
    SetWindowTextW(state.status_static, state.status_text.c_str());
}

std::wstring user_progress_text(const ProgressUpdate& update) {
    const std::wstring& text = update.text;
    if (text.rfind(L"Download:", 0) == 0 || text.rfind(L"Cache:", 0) == 0 ||
        text.rfind(L"Downloading:", 0) == 0) {
        std::wstring status = L"Downloading package - " + std::to_wstring(update.value) + L"%";
        size_t speed_separator = text.find(L" | ");
        if (speed_separator != std::wstring::npos) {
            status += L" - " + text.substr(speed_separator + 3);
        }
        return status;
    }
    if (text.rfind(L"Extracting:", 0) == 0 || text.rfind(L"Staging:", 0) == 0) {
        return L"Extracting package - " + std::to_wstring(update.value) + L"%";
    }
    if (text.rfind(L"Validation:", 0) == 0) {
        return L"Verifying written files - " + std::to_wstring(update.value) + L"%";
    }
    if (text.rfind(L"Commit:", 0) == 0 || text.rfind(L"Copy:", 0) == 0) {
        return L"Writing files - " + std::to_wstring(update.value) + L"%";
    }
    if (text.rfind(L"Finishing:", 0) == 0) {
        return L"Finishing - " + std::to_wstring(update.value) + L"%";
    }
    return text;
}

std::wstring user_log_milestone(const std::wstring& text) {
    if (text.rfind(L"Multithreaded download active", 0) == 0 ||
        text.rfind(L"Multithreaded download disabled", 0) == 0) {
        return text;
    }
    if (text == L"Downloading: package files") return L"Downloading package";
    if (text == L"Extracting: ZIP to temporary folder" || text == L"Staging: extracting ZIP before format") {
        return L"Extracting package";
    }
    if (text == L"Staging: VerifyData before format") return L"Downloaded files verified";
    if (text == L"Preparing: checking read speed") return L"Checking SD card read speed";
    if (text == L"Preparing: formatting removable drive") return L"Formatting SD card";
    if (text == L"Commit: copying verified staging to removable drive") return L"Writing files to SD card";
    if (text == L"Validation: VerifyData on removable drive") return L"Verifying written files";
    if (text == L"Finishing: ejecting removable drive") return L"Ejecting SD card";
    return L"";
}

void clear_package_view(GuiState& state) {
    if (state.package_details_edit) {
        SetWindowTextW(state.package_details_edit, L"");
    }
}

void append_detail_line(std::wstring& text, const std::wstring& label, const std::wstring& value) {
    text += label;
    text += L": ";
    text += text_or_dash(value);
    text += L"\r\n";
}

std::wstring package_details_text(const ResolveResult& result) {
    const PackageInfo& package = result.package;
    const PartInfo& part = result.resolution.resolved_part;

    std::wstring text;
    append_detail_line(text, L"Package name", package.name.empty() ? part.part_number : package.name);
    append_detail_line(text, L"Version", package.zugversion);
    append_detail_line(text, L"SD", package.sd_type + L" " + package.sd_format);
    append_detail_line(text, L"SD capacity", package.recommended_capacity_gb > 0 ? std::to_wstring(package.recommended_capacity_gb) + L" GB" : L"");
    append_detail_line(text, L"Size", byte_count_text(package.size_bytes));
    append_detail_line(text, L"Package note", package.remarks);
    return text;
}

bool extract_to_folder_checked(const GuiState& state) {
    return state.extract_to_folder &&
        SendMessageW(state.extract_to_folder, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void set_busy(GuiState& state, bool busy) {
    state.busy = busy;
    bool folder_mode = extract_to_folder_checked(state);
    EnableWindow(state.brand_combo, !busy);
    EnableWindow(state.model_combo, !busy && !state.models.empty());
    EnableWindow(state.part_edit, !busy);
    EnableWindow(state.dest_edit, !busy && folder_mode);
    EnableWindow(state.browse_button, !busy && folder_mode);
    EnableWindow(state.refresh_button, !busy);
    EnableWindow(state.drive_combo, !busy && !folder_mode);
    EnableWindow(state.refresh_drives_button, !busy && !folder_mode);
    EnableWindow(state.resolve_button, !busy && !state.brands.empty());
    EnableWindow(state.include_hidden, !busy);
    EnableWindow(state.extract_to_folder, !busy);
    EnableWindow(state.sd_card_mode, !busy);
    bool has_destination = !trim_w(get_window_text(state.dest_edit)).empty();
    bool has_drive = SendMessageW(state.drive_combo, CB_GETCURSEL, 0, 0) != CB_ERR;
    bool can_start = !busy && state.last_resolution.has_value() && (folder_mode ? has_destination : has_drive);
    EnableWindow(state.start_button, can_start);
    EnableWindow(state.cancel_button, busy && state.cancel_requested != nullptr);
    ShowWindow(state.cancel_button, busy ? SW_SHOW : SW_HIDE);
    SetWindowTextW(state.start_button,
        folder_mode ? L"Download to folder" : L"Prepare SD card");
    set_status(state, state.status_text);
}

void layout_controls(GuiState& state, int width, int height) {
    const int margin = 14;
    const int label_w = 130;
    const int row_h = 24;
    const int gap = 8;
    const int button_w = 124;
    const int footer_h = 58;
    int y = margin;
    int edit_x = margin + label_w;
    int full_w = std::max(200, width - margin * 2);
    int edit_w = std::max(240, width - edit_x - margin - button_w - gap);
    int footer_y = std::max(y + 320, height - margin - footer_h);

    MoveWindow(GetDlgItem(state.window, 2001), margin, y + 4, label_w, row_h, TRUE);
    MoveWindow(state.brand_combo, edit_x, y, edit_w, 240, TRUE);
    MoveWindow(state.refresh_button, edit_x + edit_w + gap, y, button_w, row_h, TRUE);
    y += row_h + gap;

    MoveWindow(GetDlgItem(state.window, 2005), margin, y + 4, label_w, row_h, TRUE);
    MoveWindow(state.model_combo, edit_x, y, std::max(240, width - edit_x - margin), 220, TRUE);
    y += row_h + gap;

    MoveWindow(GetDlgItem(state.window, 2002), margin, y + 4, label_w, row_h, TRUE);
    MoveWindow(state.part_edit, edit_x, y, 180, 220, TRUE);
    MoveWindow(state.resolve_button, edit_x + 188, y, button_w, row_h, TRUE);
    MoveWindow(state.include_hidden, edit_x + 188 + button_w + gap, y, 190, row_h, TRUE);
    y += row_h + gap;

    MoveWindow(GetDlgItem(state.window, 2003), margin, y + 4, label_w, row_h, TRUE);
    MoveWindow(state.sd_card_mode, edit_x, y, 84, row_h, TRUE);
    MoveWindow(state.extract_to_folder, edit_x + 88, y, 84, row_h, TRUE);
    MoveWindow(state.dest_edit, edit_x + 180, y, std::max(160, edit_w - 180), row_h, TRUE);
    MoveWindow(state.browse_button, edit_x + edit_w + gap, y, button_w, row_h, TRUE);
    y += row_h + gap;

    MoveWindow(GetDlgItem(state.window, 2004), margin, y + 4, label_w, row_h, TRUE);
    MoveWindow(state.drive_combo, edit_x, y, edit_w, 180, TRUE);
    MoveWindow(state.refresh_drives_button, edit_x + edit_w + gap, y, button_w, row_h, TRUE);
    y += row_h + gap;

    const int primary_w = 164;
    MoveWindow(state.start_button, width - margin - primary_w, y, primary_w, 32, TRUE);
    MoveWindow(state.cancel_button, width - margin - primary_w - button_w - gap, y, button_w, 32, TRUE);
    y += 38;

    footer_y = std::max(y + 240, height - margin - footer_h);
    int work_bottom = footer_y - gap;
    int available_h = std::max(210, work_bottom - y);
    int package_h = std::max(126, std::min(210, available_h / 2));
    int log_label_y = y + package_h + gap;
    int log_y = log_label_y + 20;
    int log_h = std::max(74, work_bottom - log_y);

    MoveWindow(GetDlgItem(state.window, 2006), margin, y, full_w, 18, TRUE);
    MoveWindow(state.package_details_edit, margin, y + 20, full_w, package_h - 20, TRUE);

    MoveWindow(GetDlgItem(state.window, 2008), margin, log_label_y, full_w, 18, TRUE);
    MoveWindow(state.log_edit, margin, log_y, full_w, log_h, TRUE);
    MoveWindow(state.status_static, margin, footer_y, full_w, row_h, TRUE);
    int progress_y = footer_y + row_h + 3;
    int progress_w = full_w;
    MoveWindow(state.progress, margin, progress_y, progress_w, 18, TRUE);
}

void fill_brand_combo(GuiState& state) {
    state.suppress_brand_change = true;
    SendMessageW(state.brand_combo, CB_RESETCONTENT, 0, 0);
    if (!state.brands.empty()) {
        LRESULT all_index = SendMessageW(state.brand_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All brands"));
        SendMessageW(state.brand_combo, CB_SETITEMDATA, static_cast<WPARAM>(all_index), static_cast<LPARAM>(-1));
    }
    for (size_t i = 0; i < state.brands.size(); ++i) {
        const Brand& brand = state.brands[i];
        std::wstring item = brand.name + L" (" + brand.alias_name + L")";
        LRESULT index = SendMessageW(state.brand_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        SendMessageW(state.brand_combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
    }
    if (!state.brands.empty()) {
        SendMessageW(state.brand_combo, CB_SETCURSEL, 1, 0);
    }
    state.suppress_brand_change = false;
}

void fill_model_combo(GuiState& state) {
    SendMessageW(state.model_combo, CB_RESETCONTENT, 0, 0);
    if (state.models.empty()) {
        state.models = brand_models_with_all(BrandData{});
    }
    for (size_t i = 0; i < state.models.size(); ++i) {
        const ModelInfo& model = state.models[i];
        std::wstring item = model.name.empty() ? model.id : model.name;
        LRESULT index = SendMessageW(state.model_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        SendMessageW(state.model_combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
    }
    SendMessageW(state.model_combo, CB_SETCURSEL, 0, 0);
}

void fill_drive_combo(GuiState& state) {
    SendMessageW(state.drive_combo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < state.drives.size(); ++i) {
        std::wstring item = drive_display_text(state.drives[i]);
        LRESULT index = SendMessageW(state.drive_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        SendMessageW(state.drive_combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
    }
    if (!state.drives.empty()) {
        SendMessageW(state.drive_combo, CB_SETCURSEL, 0, 0);
        if (!extract_to_folder_checked(state)) {
            SetWindowTextW(state.dest_edit, state.drives[0].root.c_str());
        }
    }
}

std::optional<RemovableDriveInfo> selected_drive(const GuiState& state) {
    LRESULT selected = SendMessageW(state.drive_combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) {
        return std::nullopt;
    }
    LRESULT data = SendMessageW(state.drive_combo, CB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
    if (data == CB_ERR || static_cast<size_t>(data) >= state.drives.size()) {
        return std::nullopt;
    }
    return state.drives[static_cast<size_t>(data)];
}

fs::path saved_extract_destination() {
    Config cfg = load_config();
    return cfg.extract_dir;
}

void sync_destination_mode(GuiState& state) {
    bool folder_mode = extract_to_folder_checked(state);
    SendMessageW(state.extract_to_folder, BM_SETCHECK, folder_mode ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.sd_card_mode, BM_SETCHECK, folder_mode ? BST_UNCHECKED : BST_CHECKED, 0);
    std::wstring destination = trim_w(get_window_text(state.dest_edit));
    if (folder_mode) {
        if (destination.empty() || is_drive_root_path(destination)) {
            fs::path saved = saved_extract_destination();
            SetWindowTextW(state.dest_edit, saved.empty() ? L"" : saved.wstring().c_str());
        }
    } else if (auto drive = selected_drive(state)) {
        SetWindowTextW(state.dest_edit, drive->root.c_str());
    }
    set_busy(state, state.busy);
}

void refresh_drives(GuiState& state) {
    state.drives = enumerate_removable_drives();
    fill_drive_combo(state);
    if (state.drives.empty()) {
        SendMessageW(state.extract_to_folder, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(state.sd_card_mode, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    sync_destination_mode(state);
    append_log(state, L"Removable drives: " + std::to_wstring(state.drives.size()));
}

std::optional<Brand> selected_brand(const GuiState& state) {
    LRESULT selected = SendMessageW(state.brand_combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) {
        return std::nullopt;
    }
    LRESULT data = SendMessageW(state.brand_combo, CB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
    if (data == CB_ERR || static_cast<size_t>(data) >= state.brands.size()) {
        return std::nullopt;
    }
    return state.brands[static_cast<size_t>(data)];
}

bool all_brands_selected(const GuiState& state) {
    LRESULT selected = SendMessageW(state.brand_combo, CB_GETCURSEL, 0, 0);
    return selected == 0 && !state.brands.empty();
}

void select_brand_in_combo(GuiState& state, const Brand& wanted) {
    for (size_t i = 0; i < state.brands.size(); ++i) {
        const Brand& brand = state.brands[i];
        if (iequals_w(brand.id, wanted.id) || iequals_w(brand.alias_name, wanted.alias_name)) {
            SendMessageW(state.brand_combo, CB_SETCURSEL, static_cast<WPARAM>(i + 1), 0);
            return;
        }
    }
}

std::wstring selected_model_id(const GuiState& state) {
    LRESULT selected = SendMessageW(state.model_combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) {
        return L"0";
    }
    LRESULT data = SendMessageW(state.model_combo, CB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
    if (data == CB_ERR || static_cast<size_t>(data) >= state.models.size()) {
        return L"0";
    }
    std::wstring id = trim_w(state.models[static_cast<size_t>(data)].id);
    return id.empty() ? L"0" : id;
}

bool include_hidden_checked(const GuiState& state) {
    return SendMessageW(state.include_hidden, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void fill_part_combo(GuiState& state) {
    std::wstring current = get_window_text(state.part_edit);
    SendMessageW(state.part_edit, CB_RESETCONTENT, 0, 0);
    bool include_hidden = include_hidden_checked(state);
    std::wstring model_id = selected_model_id(state);
    for (const PartInfo& part : state.parts) {
        bool show = part_matches_model(part, model_id);
        if (!include_hidden) {
            show = show &&
                part.is_visible &&
                trim_w(part.replacing_part_number).empty() &&
                !start_date_is_future(part);
        }
        if (show && !trim_w(part.part_number).empty()) {
            SendMessageW(state.part_edit, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(part.part_number.c_str()));
        }
    }
    if (!trim_w(current).empty()) {
        SetWindowTextW(state.part_edit, current.c_str());
    }
}

struct WorkerLaunch {
    uint64_t request_id = 0;
    std::shared_ptr<std::atomic_bool> cancel_requested;
};

class ComApartment {
public:
    explicit ComApartment(DWORD concurrency_model) {
        result_ = CoInitializeEx(nullptr, concurrency_model);
        if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE) {
            throw AppError("CoInitializeEx failed: HRESULT " +
                std::to_string(static_cast<unsigned long>(result_)));
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

private:
    HRESULT result_{};
};

using WorkerTask = std::function<void(uint64_t, const CancellationCallback&)>;

WorkerLaunch run_worker(HWND window, WorkerKind kind, WorkerTask task) {
    GuiState* state = state_from(window);
    if (!state || state->close_requested) {
        throw AppError("Cannot start a worker while the window is closing");
    }

    uint64_t request_id = ++state->next_request_id;
    if (request_id == 0) {
        request_id = ++state->next_request_id;
    }
    auto cancel_requested = std::make_shared<std::atomic_bool>(false);
    auto worker = std::make_unique<OwnedWorker>();
    worker->kind = kind;
    worker->cancel_requested = cancel_requested;
    OwnedWorker* owned = worker.get();
    state->workers.emplace(request_id, std::move(worker));

    try {
        owned->thread = std::thread([window, request_id, kind, cancel_requested, task = std::move(task)]() mutable {
            CancellationCallback cancel = [cancel_requested]() {
                if (cancel_requested->load(std::memory_order_relaxed)) {
                    throw AppError("Operation cancelled by user");
                }
            };
            try {
                ComApartment com(COINIT_MULTITHREADED);
                task(request_id, cancel);
            } catch (const std::exception& ex) {
                post_error(window, request_id, kind, exception_message(ex));
            } catch (...) {
                post_error(window, request_id, kind, L"Unknown error");
            }

            auto* finished_id = new uint64_t(request_id);
            if (!PostMessageW(window, WM_APP_WORKER_FINISHED, 0, reinterpret_cast<LPARAM>(finished_id))) {
                delete finished_id;
            }
        });
    } catch (...) {
        state->workers.erase(request_id);
        throw;
    }
    return WorkerLaunch{request_id, std::move(cancel_requested)};
}

void request_cancel_worker(GuiState& state, uint64_t request_id) {
    auto found = state.workers.find(request_id);
    if (found != state.workers.end() && found->second->cancel_requested) {
        found->second->cancel_requested->store(true, std::memory_order_relaxed);
    }
}

bool is_current_worker_event(const GuiState& state, uint64_t request_id, WorkerKind kind) {
    if (kind == WorkerKind::None) {
        return request_id == 0;
    }
    if (kind == WorkerKind::Models) {
        return request_id != 0 && request_id == state.active_model_request_id;
    }
    return request_id != 0 && request_id == state.active_request_id;
}

void reap_finished_workers(HWND window, GuiState& state) {
    bool pending = false;
    for (auto it = state.workers.begin(); it != state.workers.end();) {
        OwnedWorker& worker = *it->second;
        if (!worker.finished_notified && !state.close_requested) {
            ++it;
            continue;
        }
        if (!worker.thread.joinable() ||
            WaitForSingleObject(worker.thread.native_handle(), 0) == WAIT_OBJECT_0) {
            if (worker.thread.joinable()) {
                worker.thread.join();
            }
            it = state.workers.erase(it);
        } else {
            pending = true;
            ++it;
        }
    }

    if (pending) {
        SetTimer(window, kWorkerReapTimerId, kWorkerReapTimerMs, nullptr);
    } else {
        KillTimer(window, kWorkerReapTimerId);
    }
    if (state.close_requested && state.workers.empty()) {
        DestroyWindow(window);
    }
}

void start_load_brands(HWND window, bool refresh) {
    GuiState* state = state_from(window);
    if (!state || state->busy || state->close_requested) {
        return;
    }
    if (state->active_model_request_id != 0) {
        request_cancel_worker(*state, state->active_model_request_id);
        state->active_model_request_id = 0;
    }

    Config cfg = load_config();
    if (!catalog_source_is_configured(cfg)) {
        const std::wstring message = L"Source is not configured. Open File > Settings to select a catalog source.";
        set_status(*state, message);
        append_log(*state, message);
        return;
    }

    WorkerLaunch launch = run_worker(window, WorkerKind::Brands, [window, refresh, cfg = std::move(cfg)](uint64_t request_id, const CancellationCallback& cancel) {
        fs::path brands_path = ensure_brands_xml(cfg, refresh, cancel);
        check_cancelled(cancel);
        std::vector<Brand> brands = parse_brands_xml(brands_path);
        check_cancelled(cancel);
        auto* payload = new BrandsPayload;
        payload->request_id = request_id;
        payload->brands = std::move(brands);
        if (!PostMessageW(window, WM_APP_BRANDS, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    });
    state->active_request_id = launch.request_id;
    set_busy(*state, true);
    state->last_resolution.reset();
    clear_package_view(*state);
    SendMessageW(state->progress, PBM_SETPOS, 0, 0);
    post_log(window, launch.request_id, WorkerKind::Brands, refresh ? L"Updating brands.xml..." : L"Loading brands...");
    post_progress(window, launch.request_id, WorkerKind::Brands, 5, L"Loading brands");
}

void start_load_models(HWND window) {
    GuiState* state = state_from(window);
    if (!state || state->close_requested) {
        return;
    }
    if (state->active_model_request_id != 0) {
        request_cancel_worker(*state, state->active_model_request_id);
        state->active_model_request_id = 0;
    }
    std::optional<Brand> brand = selected_brand(*state);
    state->models = brand_models_with_all(BrandData{});
    state->parts.clear();
    fill_model_combo(*state);
    fill_part_combo(*state);
    if (!brand) {
        return;
    }

    WorkerLaunch launch = run_worker(window, WorkerKind::Models, [window, brand = *brand](uint64_t request_id, const CancellationCallback& cancel) {
        Config cfg = load_config();
        fs::path data_xml = ensure_brand_data_xml(cfg, brand, false, cancel);
        check_cancelled(cancel);
        BrandData data = parse_brand_data_xml(data_xml);
        check_cancelled(cancel);

        auto* payload = new ModelsPayload;
        payload->request_id = request_id;
        payload->brand_id = brand.id;
        payload->brand_alias = brand.alias_name;
        payload->models = brand_models_with_all(data);
        payload->parts = std::move(data.parts);
        if (!PostMessageW(window, WM_APP_MODELS, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    });
    state->active_model_request_id = launch.request_id;
}

ResolveResult resolve_package_for_brand(const Config& cfg,
    const Brand& brand,
    const std::wstring& part_number,
    const std::wstring& model_id,
    bool include_hidden,
    const CancellationCallback& cancel = {}) {
    PartResolution resolution = resolve_brand_part(cfg, brand, part_number, model_id, true, include_hidden, cancel);
    check_cancelled(cancel);
    fs::path package_xml = ensure_package_xml(cfg, brand, resolution.resolved_part.part_number, true, cancel);
    check_cancelled(cancel);
    PackageInfo package = parse_package_xml(package_xml);
    validate_package_info(package, package_xml);

    ResolveResult result;
    result.brand = brand;
    result.resolution = std::move(resolution);
    result.package = std::move(package);
    result.package_xml = std::move(package_xml);
    return result;
}

ResolveResult resolve_package_across_brands(const Config& cfg,
    const std::vector<Brand>& brands,
    const std::wstring& part_number,
    bool include_hidden,
    const CancellationCallback& cancel = {}) {
    struct BrandFailure {
        std::wstring brand;
        std::string error;
    };
    std::vector<ResolveResult> matches;
    std::vector<BrandFailure> failures;
    size_t not_found = 0;
    size_t checked = 0;
    for (const Brand& brand : brands) {
        check_cancelled(cancel);
        if (trim_w(brand.alias_name).empty()) {
            continue;
        }
        ++checked;
        try {
            matches.push_back(resolve_package_for_brand(cfg, brand, part_number, L"0", include_hidden, cancel));
        } catch (const std::exception& ex) {
            if (is_cancelled_message(ex.what())) {
                throw;
            }
            std::string error = ex.what();
            bool semantic_miss = error.rfind("Part not found in brand data.xml:", 0) == 0 ||
                error.rfind("Part is not visible and has no replacement:", 0) == 0 ||
                error.rfind("Part StartDate is in the future:", 0) == 0;
            if (semantic_miss) {
                ++not_found;
            } else {
                failures.push_back(BrandFailure{brand.name.empty() ? brand.alias_name : brand.name, std::move(error)});
            }
        }
    }

    if (matches.size() > 1) {
        std::string message = "Ambiguous part across brands: " + wide_to_utf8(part_number) + ". Matches:";
        for (const ResolveResult& match : matches) {
            std::wstring brand_name = match.brand.name.empty() ? match.brand.alias_name : match.brand.name;
            message += " " + wide_to_utf8(brand_name) +
                " [" + wide_to_utf8(match.resolution.resolved_part.part_number) + "]";
        }
        message += ". Select a brand explicitly.";
        throw AppError(message);
    }

    if (!failures.empty()) {
        std::string message = "All-brands search was incomplete for part " + wide_to_utf8(part_number) +
            ": " + std::to_string(failures.size()) + " brand checks failed";
        size_t detail_count = std::min<size_t>(failures.size(), 3);
        for (size_t i = 0; i < detail_count; ++i) {
            message += ". " + wide_to_utf8(failures[i].brand) + ": " + failures[i].error;
        }
        if (failures.size() > detail_count) {
            message += ". Additional failures: " + std::to_string(failures.size() - detail_count);
        }
        if (!matches.empty()) {
            message += ". A possible match was not selected because the search was incomplete.";
        }
        throw AppError(message);
    }

    if (matches.size() == 1) {
        return std::move(matches.front());
    }

    std::string message = "Package not found across brands: " + wide_to_utf8(part_number) +
        " (checked " + std::to_string(checked) + " brands, no match in " + std::to_string(not_found) + ")";
    throw AppError(message);
}

void start_resolve(HWND window) {
    GuiState* state = state_from(window);
    if (!state || state->busy) {
        return;
    }
    std::optional<Brand> brand = selected_brand(*state);
    bool search_all_brands = all_brands_selected(*state);
    if (!brand && !search_all_brands) {
        post_error(window, L"Brand is not selected");
        return;
    }
    std::wstring part_number = clean_part_number(get_window_text(state->part_edit));
    if (part_number.empty()) {
        post_error(window, L"Enter part number");
        return;
    }
    bool include_hidden = include_hidden_checked(*state);
    std::wstring model_id = selected_model_id(*state);
    std::vector<Brand> brands = state->brands;

    WorkerLaunch launch = run_worker(window,
        WorkerKind::Resolve,
        [window, brand, brands = std::move(brands), part_number, model_id, include_hidden, search_all_brands](uint64_t request_id, const CancellationCallback& cancel) {
            Config cfg = load_config();
            ResolveResult resolved = search_all_brands
                ? resolve_package_across_brands(cfg, brands, part_number, include_hidden, cancel)
                : resolve_package_for_brand(cfg, *brand, part_number, model_id, include_hidden, cancel);
            check_cancelled(cancel);
            resolved.request_id = request_id;
            auto* payload = new ResolveResult(std::move(resolved));
            if (!PostMessageW(window, WM_APP_RESOLVE_DONE, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        });
    state->active_request_id = launch.request_id;
    set_busy(*state, true);
    state->last_resolution.reset();
    clear_package_view(*state);
    SendMessageW(state->progress, PBM_SETPOS, 0, 0);
    post_log(window, launch.request_id, WorkerKind::Resolve, L"Searching part " + part_number + L"...");
    post_progress(window, launch.request_id, WorkerKind::Resolve, 15, L"Searching part");
}

void start_prepare(HWND window) {
    GuiState* state = state_from(window);
    if (!state || state->busy) {
        return;
    }
    std::optional<Brand> brand = selected_brand(*state);
    if (!brand) {
        post_error(window, L"Brand is not selected");
        return;
    }
    std::wstring part_number = clean_part_number(get_window_text(state->part_edit));
    std::wstring destination = trim_w(get_window_text(state->dest_edit));
    bool extract_to_folder = extract_to_folder_checked(*state);
    if (part_number.empty()) {
        post_error(window, L"Enter part number");
        return;
    }
    if (destination.empty()) {
        post_error(window, extract_to_folder ? L"Select extraction folder" : L"Select removable drive");
        return;
    }
    if (!extract_to_folder) {
        auto drive = selected_drive(*state);
        if (!drive) {
            post_error(window, L"Select removable drive");
            return;
        }
        destination = drive->root;
        SetWindowTextW(state->dest_edit, destination.c_str());
        if (!removable_drive_info(destination)) {
            post_error(window, L"Selected drive is not ready or is not removable");
            return;
        }
        std::wstring warning =
            L"All data on " + drive_display_text(*drive) + L" will be deleted.\r\n\r\n"
            L"Package: " + part_number + L"\r\n"
            L"The package will be downloaded and verified before the SD card is formatted.\r\n\r\n"
            L"Format the SD card and continue?";
        int answer = MessageBoxW(window,
            warning.c_str(),
            L"MMI Downloader",
            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
        if (answer != IDYES) {
            return;
        }
    }
    bool include_hidden = include_hidden_checked(*state);
    std::wstring model_id = selected_model_id(*state);
    WorkerLaunch launch = run_worker(window,
        WorkerKind::Prepare,
        [window, brand = *brand, part_number, model_id, destination, include_hidden, extract_to_folder](uint64_t request_id, const CancellationCallback& cancel) {
        PreparationProgressCallback progress = [window, request_id, &cancel](int value, const std::wstring& text) {
            check_cancelled(cancel);
            post_progress(window, request_id, WorkerKind::Prepare, value, text);
        };

        PreparationRequest request;
        request.brand = brand;
        request.part_number = part_number;
        request.model_id = model_id;
        request.destination = fs::path(destination);
        request.include_hidden = include_hidden;
        request.target = extract_to_folder
            ? PreparationTarget::Folder
            : PreparationTarget::RemovableDrive;

        check_cancelled(cancel);
        WindowsRemovableDriveOperations drive_operations;
        PackagePreparationService service(load_config(), drive_operations);
        PreparationOutcome outcome = service.prepare(request, progress, cancel);

        auto* payload = new PreparePayload;
        payload->request_id = request_id;
        payload->outcome = std::move(outcome);
        if (!PostMessageW(window, WM_APP_PREPARE_DONE, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    });
    state->active_request_id = launch.request_id;
    state->cancel_requested = launch.cancel_requested;
    set_busy(*state, true);
    SendMessageW(state->progress, PBM_SETPOS, 0, 0);
    clear_log(*state);
    post_log(window, launch.request_id, WorkerKind::Prepare, L"Preparing package " + part_number);
    post_progress(window, launch.request_id, WorkerKind::Prepare, 5, L"Preparing");
}

void choose_destination(HWND window) {
    GuiState* state = state_from(window);
    if (!state) {
        return;
    }

    FolderPickerResult result = pick_folder_path(
        window,
        L"Select destination folder",
        trim_w(get_window_text(state->dest_edit)));
    if (result.failure_stage == FolderPickerFailureStage::CreateDialog) {
        post_error(window, L"Could not open folder picker");
        return;
    }
    if (!result.path) {
        return;
    }

    SetWindowTextW(state->dest_edit, result.path->c_str());
    if (extract_to_folder_checked(*state)) {
        Config cfg = load_config();
        cfg.extract_dir = *result.path;
        save_config(cfg);
    }
    set_busy(*state, false);
}

void on_create(HWND window) {
    auto state = std::make_unique<GuiState>();
    state->window = window;
    state->font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HMENU menu = CreateMenu();
    HMENU popup = CreatePopupMenu();
    AppendMenuW(popup, MF_STRING, IDM_SETTINGS, L"&Settings...");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, IDM_ABOUT, L"&About...");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), L"&File");
    SetMenu(window, menu);

    HWND model_label = make_control(window, L"STATIC", L"&Model", 0, 0, 2005);
    HWND brand_label = make_control(window, L"STATIC", L"&Brand", 0, 0, 2001);
    HWND part_label = make_control(window, L"STATIC", L"&Part number", 0, 0, 2002);
    HWND dest_label = make_control(window, L"STATIC", L"Destination", 0, 0, 2003);
    HWND drive_label = make_control(window, L"STATIC", L"&SD card", 0, 0, 2004);
    HWND details_label = make_control(window, L"STATIC", L"Package details", 0, 0, 2006);
    HWND log_label = make_control(window, L"STATIC", L"Log", 0, 0, 2008);
    state->brand_combo = make_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0, IDC_BRAND);
    state->model_combo = make_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0, IDC_MODEL);
    state->part_edit = make_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL, WS_EX_CLIENTEDGE, IDC_PART);
    state->dest_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_DEST);
    state->browse_button = make_control(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON, 0, IDC_BROWSE);
    state->drive_combo = make_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0, IDC_DRIVE);
    state->refresh_drives_button = make_control(window, L"BUTTON", L"Refresh drives", BS_PUSHBUTTON, 0, IDC_REFRESH_DRIVES);
    state->refresh_button = make_control(window, L"BUTTON", L"Refresh catalog", BS_PUSHBUTTON, 0, IDC_REFRESH);
    state->resolve_button = make_control(window, L"BUTTON", L"Find package", BS_PUSHBUTTON, 0, IDC_RESOLVE);
    state->start_button = make_control(window, L"BUTTON", L"Prepare SD card", BS_DEFPUSHBUTTON, 0, IDC_START);
    state->cancel_button = make_control(window, L"BUTTON", L"Cancel", BS_PUSHBUTTON, 0, IDC_CANCEL);
    state->include_hidden = make_control(window, L"BUTTON", L"Show hidden", BS_AUTOCHECKBOX, 0, IDC_INCLUDE_HIDDEN);
    state->sd_card_mode = make_control(window, L"BUTTON", L"SD card", BS_AUTORADIOBUTTON | WS_GROUP, 0, IDC_SD_CARD_MODE);
    state->extract_to_folder = make_control(window, L"BUTTON", L"Folder", BS_AUTORADIOBUTTON, 0, IDC_EXTRACT_TO_FOLDER);
    state->package_details_edit = make_control(window,
        L"EDIT",
        L"",
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        WS_EX_CLIENTEDGE,
        IDC_PACKAGE_DETAILS);
    state->progress = make_control(window, PROGRESS_CLASSW, L"", 0, 0, IDC_PROGRESS);
    state->log_edit = make_control(window,
        L"EDIT",
        L"",
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        WS_EX_CLIENTEDGE,
        IDC_LOG);
    state->status_static = make_control(window, L"STATIC", L"Ready", SS_LEFT, 0, IDC_STATUS);

    for (HWND control : {brand_label, model_label, part_label, dest_label, drive_label, details_label, log_label, state->brand_combo, state->model_combo,
             state->part_edit, state->dest_edit, state->browse_button, state->drive_combo, state->refresh_drives_button, state->refresh_button, state->resolve_button, state->start_button,
             state->cancel_button, state->include_hidden, state->sd_card_mode, state->extract_to_folder, state->package_details_edit, state->progress, state->log_edit, state->status_static}) {
        apply_font(control, state->font);
    }
    SendMessageW(state->progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(state->sd_card_mode, BM_SETCHECK, BST_CHECKED, 0);

    state->models = brand_models_with_all(BrandData{});
    fill_model_combo(*state);
    refresh_drives(*state);

    RECT rect{};
    GetClientRect(window, &rect);
    layout_controls(*state, rect.right - rect.left, rect.bottom - rect.top);
    EnableWindow(state->resolve_button, FALSE);
    EnableWindow(state->start_button, FALSE);
    EnableWindow(state->cancel_button, FALSE);

    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
    state.release();
    start_load_brands(window, false);
}

void request_main_window_close(HWND window, GuiState& state) {
    if (state.close_requested) {
        return;
    }
    state.close_requested = true;
    for (auto& entry : state.workers) {
        if (entry.second->cancel_requested) {
            entry.second->cancel_requested->store(true, std::memory_order_relaxed);
        }
    }
    set_busy(state, true);
    EnableWindow(state.cancel_button, FALSE);
    set_status(state, state.workers.empty() ? L"Closing..." : L"Cancelling operations before close...");
    if (!state.workers.empty()) {
        append_log(state, L"Window close requested; waiting for active operations to stop");
        SetTimer(window, kWorkerReapTimerId, kWorkerReapTimerMs, nullptr);
    } else {
        DestroyWindow(window);
    }
}

void on_destroy(HWND window) {
    GuiState* state = state_from(window);
    KillTimer(window, kWorkerReapTimerId);
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    delete state;
    PostQuitMessage(0);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        try {
            on_create(window);
        } catch (const std::exception& ex) {
            MessageBoxW(window, exception_message(ex).c_str(), L"MMIDownloader", MB_ICONERROR);
            return -1;
        }
        return 0;
    case WM_SIZE: {
        GuiState* state = state_from(window);
        if (state) {
            layout_controls(*state, LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize.x = 780;
        limits->ptMinTrackSize.y = 560;
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        return handle_static_control_color(window, message, wparam, lparam);
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window,
            nullptr,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_CLOSE: {
        GuiState* state = state_from(window);
        if (state) {
            request_main_window_close(window, *state);
        } else {
            DestroyWindow(window);
        }
        return 0;
    }
    case WM_TIMER: {
        if (wparam == kWorkerReapTimerId) {
            GuiState* state = state_from(window);
            if (state) {
                reap_finished_workers(window, *state);
            }
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    case WM_COMMAND: {
        GuiState* state = state_from(window);
        if (!state) {
            return 0;
        }
        if (state->close_requested) {
            return 0;
        }
        try {
        int id = LOWORD(wparam);
        int event = HIWORD(wparam);
        if (id == IDM_SETTINGS) {
            if (state->busy) {
                return 0;
            }
            if (show_settings_dialog(window)) {
                state->last_resolution.reset();
                clear_package_view(*state);
                append_log(*state, L"Settings saved");
                start_load_brands(window, true);
            }
        } else if (id == IDM_ABOUT) {
            show_about_dialog(window);
        } else if (id == IDC_REFRESH && event == BN_CLICKED) {
            start_load_brands(window, true);
        } else if (id == IDC_REFRESH_DRIVES && event == BN_CLICKED) {
            refresh_drives(*state);
        } else if (id == IDC_RESOLVE && event == BN_CLICKED) {
            start_resolve(window);
        } else if (id == IDC_START && event == BN_CLICKED) {
            start_prepare(window);
        } else if (id == IDC_CANCEL && event == BN_CLICKED) {
            if (state->cancel_requested) {
                state->cancel_requested->store(true);
                EnableWindow(state->cancel_button, FALSE);
                set_status(*state, L"Cancelling...");
                append_log(*state, L"Operation cancellation requested");
            }
        } else if (id == IDC_PART && (event == CBN_EDITCHANGE || event == CBN_SELCHANGE)) {
            state->last_resolution.reset();
            clear_package_view(*state);
            set_busy(*state, state->busy);
        } else if (id == IDC_BROWSE && event == BN_CLICKED) {
            choose_destination(window);
        } else if (id == IDC_DRIVE && event == CBN_SELCHANGE) {
            if (!extract_to_folder_checked(*state)) {
                if (auto drive = selected_drive(*state)) {
                    SetWindowTextW(state->dest_edit, drive->root.c_str());
                    append_log(*state, L"Selected drive: " + drive_display_text(*drive));
                }
            }
            set_busy(*state, state->busy);
        } else if (id == IDC_DEST && event == EN_CHANGE) {
            set_busy(*state, state->busy);
        } else if (id == IDC_INCLUDE_HIDDEN && event == BN_CLICKED) {
            fill_part_combo(*state);
            state->last_resolution.reset();
            clear_package_view(*state);
            set_busy(*state, state->busy);
        } else if ((id == IDC_EXTRACT_TO_FOLDER || id == IDC_SD_CARD_MODE) && event == BN_CLICKED) {
            SendMessageW(state->extract_to_folder, BM_SETCHECK,
                id == IDC_EXTRACT_TO_FOLDER ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(state->sd_card_mode, BM_SETCHECK,
                id == IDC_SD_CARD_MODE ? BST_CHECKED : BST_UNCHECKED, 0);
            sync_destination_mode(*state);
            if (extract_to_folder_checked(*state) && trim_w(get_window_text(state->dest_edit)).empty()) {
                choose_destination(window);
                if (trim_w(get_window_text(state->dest_edit)).empty()) {
                    SendMessageW(state->extract_to_folder, BM_SETCHECK, BST_UNCHECKED, 0);
                    sync_destination_mode(*state);
                }
            }
        } else if (id == IDC_BRAND && event == CBN_SELCHANGE) {
            if (state->suppress_brand_change) {
                return 0;
            }
            state->last_resolution.reset();
            clear_package_view(*state);
            set_busy(*state, state->busy);
            start_load_models(window);
        } else if (id == IDC_MODEL && event == CBN_SELCHANGE) {
            fill_part_combo(*state);
            state->last_resolution.reset();
            clear_package_view(*state);
            set_busy(*state, state->busy);
        }
        } catch (const std::exception& ex) {
            std::wstring message_text = exception_message(ex);
            append_log(*state, L"Error: " + message_text);
            MessageBoxW(window, message_text.c_str(), L"MMIDownloader", MB_ICONERROR | MB_OK);
        } catch (...) {
            append_log(*state, L"Error: unknown UI failure");
            MessageBoxW(window, L"Unknown UI failure", L"MMIDownloader", MB_ICONERROR | MB_OK);
        }
        return 0;
    }
    case WM_APP_LOG: {
        std::unique_ptr<WorkerTextPayload> payload(take_payload<WorkerTextPayload>(lparam));
        GuiState* state = state_from(window);
        if (state && payload && is_current_worker_event(*state, payload->request_id, payload->kind)) {
            append_log(*state, payload->text);
        }
        return 0;
    }
    case WM_APP_ERROR: {
        std::unique_ptr<WorkerTextPayload> payload(take_payload<WorkerTextPayload>(lparam));
        GuiState* state = state_from(window);
        if (state && payload && is_current_worker_event(*state, payload->request_id, payload->kind)) {
            bool cancelled = payload->text == L"Operation cancelled by user";
            if (cancelled) {
                append_log(*state, L"Operation cancelled");
            } else {
                std::wstring user_error = L"Error - operation could not be completed";
                if (payload->kind == WorkerKind::Brands || payload->kind == WorkerKind::Models) {
                    user_error = L"Error - catalog could not be loaded";
                } else if (payload->kind == WorkerKind::Resolve) {
                    user_error = L"Error - package could not be found";
                }
                append_log(*state, user_error + L": " + payload->text);
            }

            if (payload->kind == WorkerKind::Models) {
                state->active_model_request_id = 0;
            } else if (payload->kind != WorkerKind::None) {
                state->active_request_id = 0;
                state->cancel_requested.reset();
                if (!state->close_requested) {
                    set_status(*state, cancelled ? L"Operation cancelled" : L"Error");
                    SendMessageW(state->progress, PBM_SETPOS, 0, 0);
                    set_busy(*state, false);
                }
            } else if (!state->close_requested) {
                set_status(*state, L"Error");
            }
        }
        return 0;
    }
    case WM_APP_PROGRESS: {
        std::unique_ptr<ProgressUpdate> update(take_payload<ProgressUpdate>(lparam));
        GuiState* state = state_from(window);
        if (state && update && !state->close_requested &&
            is_current_worker_event(*state, update->request_id, update->kind)) {
            SendMessageW(state->progress, PBM_SETPOS, static_cast<WPARAM>(update->value), 0);
            set_status(*state, user_progress_text(*update));
            if (std::wstring milestone = user_log_milestone(update->text); !milestone.empty()) {
                append_log(*state, milestone);
            }
        }
        return 0;
    }
    case WM_APP_BRANDS: {
        std::unique_ptr<BrandsPayload> payload(take_payload<BrandsPayload>(lparam));
        GuiState* state = state_from(window);
        if (state && payload && payload->request_id == state->active_request_id) {
            state->active_request_id = 0;
            if (state->close_requested) {
                return 0;
            }
            state->brands = std::move(payload->brands);
            fill_brand_combo(*state);
            SendMessageW(state->progress, PBM_SETPOS, 100, 0);
            append_log(*state, L"Brands loaded: " + std::to_wstring(state->brands.size()));
            set_status(*state, L"Brands loaded");
            set_busy(*state, false);
            start_load_models(window);
        }
        return 0;
    }
    case WM_APP_MODELS: {
        std::unique_ptr<ModelsPayload> payload(take_payload<ModelsPayload>(lparam));
        GuiState* state = state_from(window);
        if (state && payload && payload->request_id == state->active_model_request_id) {
            state->active_model_request_id = 0;
            if (state->close_requested) {
                return 0;
            }
            std::optional<Brand> current = selected_brand(*state);
            if (current &&
                iequals_w(current->id, payload->brand_id) &&
                iequals_w(current->alias_name, payload->brand_alias)) {
                state->models = std::move(payload->models);
                state->parts = std::move(payload->parts);
                fill_model_combo(*state);
                fill_part_combo(*state);
                set_busy(*state, state->busy);
            }
        }
        return 0;
    }
    case WM_APP_RESOLVE_DONE: {
        std::unique_ptr<ResolveResult> result(take_payload<ResolveResult>(lparam));
        GuiState* state = state_from(window);
        if (state && result && result->request_id == state->active_request_id) {
            state->active_request_id = 0;
            if (state->close_requested) {
                return 0;
            }
            select_brand_in_combo(*state, result->brand);
            std::wstring info = package_details_text(*result);
            SetWindowTextW(state->package_details_edit, info.c_str());
            SetWindowTextW(state->part_edit, result->resolution.resolved_part.part_number.c_str());
            append_log(*state, L"Part found: " + result->resolution.resolved_part.part_number);
            state->last_resolution = *result;
            SendMessageW(state->progress, PBM_SETPOS, 100, 0);
            set_status(*state, L"Part found");
            set_busy(*state, false);
        }
        return 0;
    }
    case WM_APP_PREPARE_DONE: {
        std::unique_ptr<PreparePayload> result(take_payload<PreparePayload>(lparam));
        GuiState* state = state_from(window);
        if (state && result && result->request_id == state->active_request_id) {
            state->active_request_id = 0;
            if (state->close_requested) {
                state->cancel_requested.reset();
                return 0;
            }
            SendMessageW(state->progress, PBM_SETPOS, 100, 0);
            append_log(*state,
                result->outcome.extracted_to_folder
                    ? L"Completed - files are ready in the selected folder"
                    : L"Completed - the SD card is ready");
            if (result->outcome.success_log_failed) {
                append_log(*state, L"Warning: operation completed, but the success log could not be written");
            }
            set_status(*state, L"Completed");
            state->cancel_requested.reset();
            set_busy(*state, false);
        }
        return 0;
    }
    case WM_APP_WORKER_FINISHED: {
        std::unique_ptr<uint64_t> request_id(take_payload<uint64_t>(lparam));
        GuiState* state = state_from(window);
        if (state && request_id) {
            auto found = state->workers.find(*request_id);
            if (found != state->workers.end()) {
                found->second->finished_notified = true;
            }
            reap_finished_workers(window, *state);
        }
        return 0;
    }
    case WM_DESTROY:
        on_destroy(window);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

HICON create_app_icon(int size) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen,
        reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !pixels) {
        if (color) DeleteObject(color);
        return LoadIconW(nullptr, IDI_APPLICATION);
    }

    auto* argb = static_cast<uint32_t*>(pixels);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t value = 0xFF30343B;
            const int border = std::max(1, size / 10);
            const bool blue_bar = y >= size / 2 && y < size - border &&
                x >= border && x < size - border && ((x / std::max(1, size / 8)) % 2 == 0);
            const bool white_m = y >= border && y < size / 2 &&
                (x == border || x == size - border - 1 || x == y || x == size - y - 1);
            if (blue_bar) value = 0xFF168CE6;
            if (white_m) value = 0xFFFFFFFF;
            argb[y * size + x] = value;
        }
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color;
    icon_info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&icon_info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

HICON load_app_icon(HINSTANCE instance, int size) {
    HICON icon = reinterpret_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(kAppIconResourceId),
        IMAGE_ICON,
        size,
        size,
        LR_DEFAULTCOLOR));
    return icon ? icon : create_app_icon(size);
}

int run_application(HINSTANCE instance, int show_command) {
    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&common_controls);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hIcon = load_app_icon(instance, 32);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    window_class.hIconSm = load_app_icon(instance, 16);
    if (!RegisterClassExW(&window_class)) {
        throw AppError("RegisterClassEx failed: " + format_win32_error(GetLastError()));
    }

    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        L"MMI Downloader",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        860,
        560,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!window) {
        throw AppError("CreateWindowEx failed: " + format_win32_error(GetLastError()));
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    for (;;) {
        BOOL message_result = GetMessageW(&message, nullptr, 0, 0);
        if (message_result == -1) {
            throw AppError("GetMessage failed: " + format_win32_error(GetLastError()));
        }
        if (message_result == 0) {
            return static_cast<int>(message.wParam);
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

} // namespace

int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE,
    _In_ PWSTR,
    _In_ int show_command) {
    try {
        ComApartment com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        return run_application(instance, show_command);
    } catch (const std::exception& ex) {
        MessageBoxW(nullptr, exception_message(ex).c_str(), L"MMIDownloader", MB_ICONERROR | MB_OK);
        return 1;
    } catch (...) {
        MessageBoxW(nullptr, L"Unknown startup error", L"MMIDownloader", MB_ICONERROR | MB_OK);
        return 1;
    }
}
