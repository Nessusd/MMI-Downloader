#include "core.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <commctrl.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <shobjidl.h>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <winioctl.h>
#include <wrl/client.h>

namespace fs = std::filesystem;

namespace {

using namespace mmid;

constexpr wchar_t kWindowClass[] = L"MMIDownloaderWindow";
constexpr wchar_t kSettingsClass[] = L"MMIDownloaderSettingsWindow";
constexpr wchar_t kSourceClass[] = L"MMIDownloaderSourceWindow";
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

enum SettingsControlId {
    IDC_SETTINGS_SOURCE_TEXT = 3001,
    IDC_SETTINGS_SOURCE_ADD = 3002,
    IDC_SETTINGS_METADATA = 3004,
    IDC_SETTINGS_TEMP = 3005,
    IDC_SETTINGS_OK = 3006,
    IDC_SETTINGS_CANCEL = 3007,
    IDC_SETTINGS_LOGGING = 3008,
    IDC_SETTINGS_LOG = 3009,
    IDC_SETTINGS_LOG_ADD = 3010,
    IDC_SETTINGS_METADATA_BROWSE = 3012,
    IDC_SETTINGS_TEMP_BROWSE = 3013
};

enum SourceControlId {
    IDC_SOURCE_USE_LOCAL = 3101,
    IDC_SOURCE_LOCAL = 3102,
    IDC_SOURCE_NETWORK = 3103,
    IDC_SOURCE_USERNAME = 3104,
    IDC_SOURCE_PASSWORD = 3105,
    IDC_SOURCE_OK = 3106,
    IDC_SOURCE_CANCEL = 3107,
    IDC_SOURCE_TEST = 3108,
    IDC_SOURCE_LOCAL_ADD = 3109,
    IDC_SOURCE_FTP_MODE = 3110
};

struct DriveInfoNative {
    std::wstring root;
    std::wstring label;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
};

struct ResolveResult {
    uint64_t request_id = 0;
    Brand brand;
    PartResolution resolution;
    PackageInfo package;
    fs::path package_xml;
};

struct PrepareResult {
    uint64_t request_id = 0;
    bool extracted_to_folder = false;
    bool success_log_failed = false;
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
    std::vector<DriveInfoNative> drives;
    std::optional<ResolveResult> last_resolution;
    std::shared_ptr<std::atomic_bool> cancel_requested;
    std::unordered_map<uint64_t, std::unique_ptr<OwnedWorker>> workers;
    uint64_t next_request_id = 0;
    uint64_t active_request_id = 0;
    uint64_t active_model_request_id = 0;
    bool close_requested = false;
    bool suppress_brand_change = false;
};

struct SettingsDialogState {
    HWND owner = nullptr;
    HWND source_static = nullptr;
    HWND metadata_edit = nullptr;
    HWND temp_edit = nullptr;
    HWND logging_check = nullptr;
    HWND log_edit = nullptr;
    HWND log_add_button = nullptr;
    HFONT font = nullptr;
    bool saved = false;
};

struct SourceDialogState {
    HWND owner = nullptr;
    HWND use_local_check = nullptr;
    HWND local_edit = nullptr;
    HWND local_add_button = nullptr;
    HWND network_edit = nullptr;
    HWND ftp_mode_combo = nullptr;
    HWND username_edit = nullptr;
    HWND password_edit = nullptr;
    HWND test_button = nullptr;
    HFONT font = nullptr;
    std::wstring initial_network;
    bool loaded_legacy_webdav = false;
    bool saved = false;
};

std::wstring exception_to_wide(const std::exception& ex) {
    try {
        return utf8_to_wide(ex.what());
    } catch (...) {
        int needed = MultiByteToWideChar(CP_ACP, 0, ex.what(), -1, nullptr, 0);
        if (needed <= 0) {
            return L"Unknown error";
        }
        std::vector<wchar_t> buffer(static_cast<size_t>(needed));
        if (MultiByteToWideChar(CP_ACP, 0, ex.what(), -1, buffer.data(), needed) <= 0) {
            return L"Unknown error";
        }
        return std::wstring(buffer.data());
    }
}

std::wstring get_window_text(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<size_t>(length));
    return text;
}

std::wstring clean_part_number(std::wstring value) {
    value = trim_w(std::move(value));
    value.erase(std::remove(value.begin(), value.end(), L'.'), value.end());
    return value;
}

std::wstring normalize_drive_root(std::wstring value) {
    value = trim_w(std::move(value));
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.size() == 2 && value[1] == L':') {
        value += L'\\';
    }
    if (value.size() >= 3) {
        return value.substr(0, 3);
    }
    return value;
}

bool is_drive_root_path(const std::wstring& value) {
    std::wstring root = normalize_drive_root(value);
    return root.size() == 3 &&
        ((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')) &&
        root[1] == L':' &&
        root[2] == L'\\';
}

std::wstring volume_root_for_path(fs::path path, const char* context) {
    std::error_code error;
    path = fs::absolute(path, error);
    if (error) {
        throw AppError(std::string("Could not resolve ") + context + ": " + error.message());
    }

    while (!fs::exists(path, error)) {
        if (error) {
            throw AppError(std::string("Could not inspect ") + context + ": " + error.message());
        }
        fs::path parent = path.parent_path();
        if (parent.empty() || parent == path) {
            throw AppError(std::string("Could not find an existing parent for ") + context);
        }
        path = std::move(parent);
    }

    std::vector<wchar_t> root(32768, L'\0');
    if (!GetVolumePathNameW(path.wstring().c_str(), root.data(), static_cast<DWORD>(root.size()))) {
        throw AppError(std::string("GetVolumePathName failed for ") + context + ": " +
            format_win32_error(GetLastError()));
    }
    return trim_w(root.data());
}

void reject_temp_directory_on_destination_volume(const Config& cfg, const std::wstring& destination) {
    std::wstring temp_volume = volume_root_for_path(cfg.temp_dir, "temporary directory");
    std::wstring destination_volume = volume_root_for_path(fs::path(destination), "destination drive");
    if (iequals_w(temp_volume, destination_volume)) {
        throw AppError("Temporary directory must not be located on the selected SD card because the card will be formatted");
    }
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

std::optional<DriveInfoNative> removable_drive_info(const std::wstring& root_value) {
    std::wstring root = normalize_drive_root(root_value);
    if (!is_drive_root_path(root)) {
        return std::nullopt;
    }
    if (GetDriveTypeW(root.c_str()) != DRIVE_REMOVABLE) {
        return std::nullopt;
    }

    ULARGE_INTEGER free_bytes{};
    ULARGE_INTEGER total_bytes{};
    ULARGE_INTEGER total_free_bytes{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &free_bytes, &total_bytes, &total_free_bytes)) {
        return std::nullopt;
    }

    wchar_t volume_name[MAX_PATH]{};
    GetVolumeInformationW(root.c_str(), volume_name, MAX_PATH,
        nullptr, nullptr, nullptr, nullptr, 0);

    DriveInfoNative info;
    info.root = root;
    info.label = volume_name;
    info.total_bytes = total_bytes.QuadPart;
    info.free_bytes = total_free_bytes.QuadPart;
    return info;
}

std::vector<DriveInfoNative> enumerate_removable_drives() {
    DWORD needed = GetLogicalDriveStringsW(0, nullptr);
    if (needed == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1, L'\0');
    DWORD written = GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (written == 0) {
        return {};
    }

    std::vector<DriveInfoNative> drives;
    const wchar_t* current = buffer.data();
    while (*current) {
        if (auto info = removable_drive_info(current)) {
            drives.push_back(*info);
        }
        current += wcslen(current) + 1;
    }
    return drives;
}

std::wstring drive_display_text(const DriveInfoNative& drive) {
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

void validate_removable_destination(const std::wstring& destination, const PackageInfo& package) {
    if (!is_drive_root_path(destination)) {
        return;
    }
    auto info = removable_drive_info(destination);
    if (!info) {
        throw AppError("Selected destination is not a ready removable drive: " + wide_to_utf8(destination));
    }
    if (package.recommended_capacity_gb > 0) {
        uint64_t recommended = static_cast<uint64_t>(package.recommended_capacity_gb) * 1024ull * 1024ull * 1024ull;
        uint64_t threshold = recommended - recommended / 10;
        if (info->total_bytes <= threshold) {
            throw AppError("Removable drive capacity is below package recommendation: " +
                wide_to_utf8(info->root) + " total=" + wide_to_utf8(bytes_to_gb_text(info->total_bytes)) +
                ", required=" + std::to_string(package.recommended_capacity_gb) + " GB");
        }
    }
}

std::wstring filesystem_for_drive(const PackageInfo& package, const DriveInfoNative& drive) {
    std::wstring recommended = trim_w(package.sd_format);
    if (recommended.empty()) {
        recommended = L"FAT32";
    }

    std::wstring canonical;
    if (iequals_w(recommended, L"FAT")) {
        canonical = L"FAT";
    } else if (iequals_w(recommended, L"FAT32")) {
        canonical = L"FAT32";
    } else if (iequals_w(recommended, L"exFAT")) {
        canonical = L"exFAT";
    } else if (iequals_w(recommended, L"NTFS")) {
        canonical = L"NTFS";
    } else {
        throw AppError("Unsupported SD filesystem in package metadata: " + wide_to_utf8(recommended));
    }

    if (drive.total_bytes > 34359738368ull && canonical == L"FAT32") {
        return L"exFAT";
    }
    return canonical;
}

DWORD run_hidden_process(const std::wstring& executable,
    const std::wstring& arguments,
    const CancellationCallback& cancel,
    uint64_t timeout_ms) {
    struct ProcessInformationGuard {
        PROCESS_INFORMATION value{};

        ProcessInformationGuard() = default;
        ProcessInformationGuard(const ProcessInformationGuard&) = delete;
        ProcessInformationGuard& operator=(const ProcessInformationGuard&) = delete;

        ~ProcessInformationGuard() {
            close();
        }

        void close() noexcept {
            if (value.hThread) {
                CloseHandle(value.hThread);
                value.hThread = nullptr;
            }
            if (value.hProcess) {
                CloseHandle(value.hProcess);
                value.hProcess = nullptr;
            }
        }
    } process_guard;

    std::wstring command_line = L"\"" + executable + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION& process = process_guard.value;
    if (!CreateProcessW(nullptr,
        command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process)) {
        throw AppError("CreateProcess failed for " + wide_to_utf8(executable) + ": " + format_win32_error(GetLastError()));
    }
    auto terminate_process_and_wait = [&]() {
        if (!process.hProcess) {
            return;
        }

        DWORD initial_wait = WaitForSingleObject(process.hProcess, 0);
        if (initial_wait == WAIT_OBJECT_0) {
            return;
        }

        // TerminateProcess is asynchronous. Keep the worker alive until the
        // process handle is signalled; WM_CLOSE waits for this worker without
        // blocking the UI thread, so format.com cannot outlive the GUI.
        (void)TerminateProcess(process.hProcess, ERROR_CANCELLED);
        for (;;) {
            DWORD wait_result = WaitForSingleObject(process.hProcess, 1000);
            if (wait_result == WAIT_OBJECT_0) {
                return;
            }

            DWORD exit_code = STILL_ACTIVE;
            if (GetExitCodeProcess(process.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
                return;
            }

            // A failed termination request is not treated as cancellation
            // completion. Retry while the process is demonstrably active.
            (void)TerminateProcess(process.hProcess, ERROR_CANCELLED);
        }
    };

    uint64_t started = GetTickCount64();
    for (;;) {
        DWORD wait_result = WaitForSingleObject(process.hProcess, 200);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (wait_result == WAIT_FAILED) {
            DWORD error = GetLastError();
            terminate_process_and_wait();
            throw AppError("WaitForSingleObject failed for " + wide_to_utf8(executable) + ": " + format_win32_error(error));
        }
        try {
            check_cancelled(cancel);
        } catch (...) {
            terminate_process_and_wait();
            throw;
        }
        if (timeout_ms > 0 && GetTickCount64() - started >= timeout_ms) {
            terminate_process_and_wait();
            throw AppError("Process timed out: " + wide_to_utf8(executable));
        }
    }

    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        DWORD error = GetLastError();
        throw AppError("GetExitCodeProcess failed for " + wide_to_utf8(executable) + ": " + format_win32_error(error));
    }
    return exit_code;
}

void wait_for_ready_removable_drive(const std::wstring& root, const CancellationCallback& cancel) {
    for (int i = 0; i < 60; ++i) {
        check_cancelled(cancel);
        if (removable_drive_info(root)) {
            return;
        }
        Sleep(500);
    }
    throw AppError("Removable drive did not become ready after format: " + wide_to_utf8(root));
}

void format_removable_drive(const std::wstring& root,
    const PackageInfo& package,
    const CancellationCallback& cancel) {
    auto drive = removable_drive_info(root);
    if (!drive) {
        throw AppError("Cannot format non-removable or not-ready drive: " + wide_to_utf8(root));
    }
    std::wstring filesystem = filesystem_for_drive(package, *drive);

    wchar_t system_dir[MAX_PATH]{};
    UINT system_len = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (system_len == 0 || system_len >= MAX_PATH) {
        throw AppError("GetSystemDirectory failed: " + format_win32_error(GetLastError()));
    }
    fs::path format_path = fs::path(system_dir) / L"format.com";
    std::wstring arguments = L"/FS:" + filesystem + L" /V:SDCREATOR /Q /Y ";
    arguments += towupper(root[0]);
    arguments += L":";

    constexpr uint64_t format_timeout_ms = 15ull * 60ull * 1000ull;
    DWORD exit_code = run_hidden_process(format_path.wstring(), arguments, cancel, format_timeout_ms);
    if (exit_code != 0) {
        throw AppError("format.com failed for " + wide_to_utf8(root) + " with exit code " + std::to_string(exit_code));
    }
    wait_for_ready_removable_drive(root, cancel);
}

class WinHandle {
public:
    explicit WinHandle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    ~WinHandle() {
        close();
    }
    HANDLE get() const {
        return handle_;
    }
    explicit operator bool() const {
        return handle_ != INVALID_HANDLE_VALUE;
    }
    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_;
};

fs::path create_speed_test_source_file(uint64_t size_bytes, const CancellationCallback& cancel) {
    fs::path temp = fs::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
        fs::path path = temp / fs::path(L"mmidownloader-speed-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetCurrentThreadId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(attempt) + L".bin");
        WinHandle file(CreateFileW(path.wstring().c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_FLAG_WRITE_THROUGH,
            nullptr));
        if (!file) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                continue;
            }
            throw AppError("Could not create speed test source file: " + printable_path(path) +
                " (" + format_win32_error(error) + ")");
        }

        try {
            std::vector<uint8_t> buffer(1024 * 1024);
            for (size_t i = 0; i < buffer.size(); ++i) {
                buffer[i] = static_cast<uint8_t>(i);
            }
            uint64_t written_total = 0;
            while (written_total < size_bytes) {
                check_cancelled(cancel);
                DWORD to_write = static_cast<DWORD>(std::min<uint64_t>(buffer.size(), size_bytes - written_total));
                DWORD written = 0;
                if (!WriteFile(file.get(), buffer.data(), to_write, &written, nullptr) || written != to_write) {
                    throw AppError("Could not write speed test source file: " + format_win32_error(GetLastError()));
                }
                written_total += written;
            }
            if (!FlushFileBuffers(file.get())) {
                throw AppError("Could not flush speed test source file: " + format_win32_error(GetLastError()));
            }
            return path;
        } catch (...) {
            file.close();
            (void)DeleteFileW(path.wstring().c_str());
            throw;
        }
    }
    throw AppError("Could not allocate a unique speed test source filename");
}

void copy_speed_test_file_to_drive(const fs::path& source, const fs::path& target) {
    if (!CopyFileW(source.wstring().c_str(), target.wstring().c_str(), TRUE)) {
        throw AppError("Could not copy speed test file to drive: " + printable_path(target) +
            " (" + format_win32_error(GetLastError()) + ")");
    }
    try {
        WinHandle target_file(CreateFileW(target.wstring().c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr));
        if (!target_file) {
            throw AppError("Could not open copied speed test file for flush: " + printable_path(target) +
                " (" + format_win32_error(GetLastError()) + ")");
        }
        if (!FlushFileBuffers(target_file.get())) {
            throw AppError("Could not flush copied speed test file: " + printable_path(target) +
                " (" + format_win32_error(GetLastError()) + ")");
        }
    } catch (...) {
        (void)DeleteFileW(target.wstring().c_str());
        throw;
    }
}

uint64_t read_file_no_buffering(const fs::path& path, const CancellationCallback& cancel) {
    WinHandle file(CreateFileW(path.wstring().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file) {
        throw AppError("Could not open speed test file for read: " + printable_path(path) +
            " (" + format_win32_error(GetLastError()) + ")");
    }

    constexpr size_t buffer_size = 8 * 1024 * 1024;
    void* buffer = VirtualAlloc(nullptr, buffer_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) {
        throw AppError("VirtualAlloc failed for speed test buffer: " + format_win32_error(GetLastError()));
    }

    uint64_t total = 0;
    try {
        for (;;) {
            check_cancelled(cancel);
            DWORD read = 0;
            if (!ReadFile(file.get(), buffer, static_cast<DWORD>(buffer_size), &read, nullptr)) {
                throw AppError("Speed test read failed: " + format_win32_error(GetLastError()));
            }
            if (read == 0) {
                break;
            }
            total += read;
        }
    } catch (...) {
        VirtualFree(buffer, 0, MEM_RELEASE);
        throw;
    }
    VirtualFree(buffer, 0, MEM_RELEASE);
    return total;
}

void remove_speed_test_file_confirmed(const fs::path& path, const char* description) {
    std::error_code ec;
    (void)fs::remove(path, ec);
    if (ec) {
        throw AppError(std::string("Could not remove ") + description + ": " +
            printable_path(path) + " (" + ec.message() + ")");
    }

    ec.clear();
    bool still_exists = fs::exists(path, ec);
    if (ec) {
        throw AppError(std::string("Could not confirm removal of ") + description + ": " +
            printable_path(path) + " (" + ec.message() + ")");
    }
    if (still_exists) {
        throw AppError(std::string("Removal of ") + description + " was not completed: " + printable_path(path));
    }
}

void remove_speed_test_file_best_effort(const fs::path& path) noexcept {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    (void)fs::remove(path, ec);
}

double check_removable_read_speed(const std::wstring& root_value, const CancellationCallback& cancel) {
    std::wstring root = normalize_drive_root(root_value);
    auto drive = removable_drive_info(root);
    if (!drive) {
        throw AppError("Cannot run speed check on non-removable or not-ready drive: " + wide_to_utf8(root));
    }

    constexpr uint64_t test_size = 50ull * 1024ull * 1024ull;
    if (drive->free_bytes < test_size + 16ull * 1024ull * 1024ull) {
        throw AppError("Not enough free space for speed check on " + wide_to_utf8(root));
    }

    fs::path source;
    fs::path target;
    bool target_created = false;
    try {
        source = create_speed_test_source_file(test_size, cancel);
        target = fs::path(root) / source.filename();
        copy_speed_test_file_to_drive(source, target);
        target_created = true;

        auto start = std::chrono::steady_clock::now();
        uint64_t read_bytes = read_file_no_buffering(target, cancel);
        auto finish = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = finish - start;
        if (read_bytes != test_size || elapsed.count() <= 0.0) {
            throw AppError("Speed check read size mismatch");
        }
        double speed = std::round((static_cast<double>(read_bytes) / 1024.0 / 1024.0 / elapsed.count()) * 100.0) / 100.0;
        remove_speed_test_file_confirmed(target, "speed test file on removable drive");
        target_created = false;
        remove_speed_test_file_confirmed(source, "speed test source file");
        source.clear();
        return speed;
    } catch (...) {
        if (target_created && !target.empty()) {
            remove_speed_test_file_best_effort(target);
        }
        if (!source.empty()) {
            remove_speed_test_file_best_effort(source);
        }
        throw;
    }
}

bool rename_removable_drive(const std::wstring& root, const std::wstring& part_number) {
    if (!is_drive_root_path(root)) {
        return false;
    }
    std::wstring label = part_number.substr(0, std::min<size_t>(11, part_number.size()));
    return SetVolumeLabelW(normalize_drive_root(root).c_str(), label.c_str()) != FALSE;
}

bool device_io_control_bool(HANDLE handle, DWORD code, void* input = nullptr, DWORD input_size = 0) {
    DWORD returned = 0;
    return DeviceIoControl(handle, code, input, input_size, nullptr, 0, &returned, nullptr) != FALSE;
}

bool wait_for_drive_removed(const std::wstring& root) {
    for (int i = 0; i < 20; ++i) {
        if (!removable_drive_info(root)) {
            return true;
        }
        Sleep(500);
    }
    return !removable_drive_info(root).has_value();
}

void eject_removable_drive(const std::wstring& root_value) {
    std::wstring root = normalize_drive_root(root_value);
    if (!removable_drive_info(root)) {
        throw AppError("Cannot eject non-removable or not-ready drive: " + wide_to_utf8(root));
    }

    std::wstring volume_path = L"\\\\.\\";
    volume_path += towupper(root[0]);
    volume_path += L":";

    WinHandle volume(CreateFileW(volume_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr));
    if (!volume) {
        throw AppError("CreateFile failed for " + wide_to_utf8(volume_path) + ": " + format_win32_error(GetLastError()));
    }

    bool locked = false;
    for (int i = 0; i < 20; ++i) {
        if (device_io_control_bool(volume.get(), FSCTL_LOCK_VOLUME)) {
            locked = true;
            break;
        }
        Sleep(500);
    }
    if (!locked) {
        throw AppError("Could not lock volume before eject: " + wide_to_utf8(root) + " (" + format_win32_error(GetLastError()) + ")");
    }

    if (!device_io_control_bool(volume.get(), FSCTL_DISMOUNT_VOLUME)) {
        throw AppError("Could not dismount volume before eject: " + wide_to_utf8(root) + " (" + format_win32_error(GetLastError()) + ")");
    }

    PREVENT_MEDIA_REMOVAL removal{};
    removal.PreventMediaRemoval = FALSE;
    (void)device_io_control_bool(volume.get(), IOCTL_STORAGE_MEDIA_REMOVAL, &removal, sizeof(removal));

    if (!device_io_control_bool(volume.get(), IOCTL_STORAGE_EJECT_MEDIA)) {
        throw AppError("Could not eject removable drive: " + wide_to_utf8(root) + " (" + format_win32_error(GetLastError()) + ")");
    }

    volume.close();
    if (!wait_for_drive_removed(root)) {
        throw AppError("Eject command succeeded but drive is still mounted: " + wide_to_utf8(root));
    }
}

using ProgressCallback = std::function<void(int, const std::wstring&)>;

struct PreparePackageContext {
    PartResolution resolution;
    fs::path package_xml;
    PackageInfo package;
    std::wstring report_file;
};

struct PreparePayloadContext {
    fs::path zip_payload;
    fs::path staging_dir;
    ZipExtractResult extract;
    size_t payload_count = 0;
};

PreparePackageContext resolve_prepare_package(const Config& cfg,
    const Brand& brand,
    const std::wstring& part_number,
    const std::wstring& model_id,
    bool include_hidden,
    const ProgressCallback& progress,
    const CancellationCallback& cancel = {}) {
    progress(15, L"Preparing: searching part");
    PartResolution resolution = resolve_brand_part(cfg, brand, part_number, model_id, true, include_hidden, cancel);

    check_cancelled(cancel);
    progress(25, L"Downloading: package XML");
    fs::path package_xml = ensure_package_xml(cfg, brand, resolution.resolved_part.part_number, true, cancel);
    check_cancelled(cancel);
    PackageInfo package = parse_package_xml(package_xml);

    PreparePackageContext context;
    context.resolution = std::move(resolution);
    context.package_xml = std::move(package_xml);
    context.package = std::move(package);
    if (!context.package.checksums.empty()) {
        context.report_file = fs::path(context.package.checksums.front().filename).filename().wstring();
    }
    return context;
}

PreparePayloadContext download_extract_and_verify_payload(const Config& cfg,
    const Brand& brand,
    const PackageInfo& package,
    const std::wstring& destination,
    OperationStatus& failure_status,
    const ProgressCallback& progress,
    const CancellationCallback& cancel) {
    check_cancelled(cancel);
    progress(45, L"Downloading: package files");
    failure_status = OperationStatus::BtacNoWebDavConnection;
    std::vector<fs::path> payload_files = download_payload_files(cfg,
        brand.alias_name,
        package,
        [&progress](size_t index, size_t total, const fs::path& path, bool downloaded, double network_mbps) {
            int value = 45;
            if (total > 0) {
                value += static_cast<int>((index * 20) / total);
            }
            std::wstring text = std::wstring(downloaded ? L"Download: " : L"Cache: ") +
                path.filename().wstring() +
                L" [" + std::to_wstring(index) + L"/" + std::to_wstring(total) + L"]";
            if (downloaded && network_mbps > 0.0) {
                std::wostringstream speed;
                speed << std::fixed << std::setprecision(2) << network_mbps << L" MB/s";
                text += L" | " + speed.str();
            }
            progress(value, text);
        },
        [&progress](bool enabled, size_t workers) {
            (void)workers;
            progress(45, enabled ? L"Multithreaded download active" : L"Multithreaded download disabled");
        },
        cancel);
    check_cancelled(cancel);
    failure_status = OperationStatus::PackageVerifyFailed;
    fs::path zip_payload = select_zip_payload(payload_files);

    progress(70, L"Extracting: ZIP to temporary folder");
    failure_status = OperationStatus::PackageExtractFailed;
    std::wstring password = decrypt_legacy_package_password(package.password);
    ZipExtractResult extract = extract_package_to_destination_with_staging(cfg,
        zip_payload,
        destination,
        package,
        password,
        [&progress, &failure_status](const std::wstring& text) {
            if (text.find(L"Validation:") == 0 || text.find(L"Staging: verify") == 0) {
                failure_status = OperationStatus::PackageVerifyFailed;
            } else if (text.find(L"Copy:") == 0) {
                failure_status = OperationStatus::PackageExtractFailed;
            }
            progress(80, text);
        },
        cancel);

    PreparePayloadContext context;
    context.zip_payload = std::move(zip_payload);
    context.extract = extract;
    context.payload_count = payload_files.size();
    return context;
}

PreparePayloadContext download_and_stage_payload(const Config& cfg,
    const Brand& brand,
    const PackageInfo& package,
    OperationStatus& failure_status,
    const ProgressCallback& progress,
    const CancellationCallback& cancel) {
    check_cancelled(cancel);
    progress(38, L"Downloading: package files");
    failure_status = OperationStatus::BtacNoWebDavConnection;
    std::vector<fs::path> payload_files = download_payload_files(cfg,
        brand.alias_name,
        package,
        [&progress](size_t index, size_t total, const fs::path& path, bool downloaded, double network_mbps) {
            int value = 38;
            if (total > 0) {
                value += static_cast<int>((index * 22) / total);
            }
            std::wstring text = std::wstring(downloaded ? L"Download: " : L"Cache: ") +
                path.filename().wstring() +
                L" [" + std::to_wstring(index) + L"/" + std::to_wstring(total) + L"]";
            if (downloaded && network_mbps > 0.0) {
                std::wostringstream speed;
                speed << std::fixed << std::setprecision(2) << network_mbps << L" MB/s";
                text += L" | " + speed.str();
            }
            progress(value, text);
        },
        [&progress](bool enabled, size_t workers) {
            (void)workers;
            progress(38, enabled ? L"Multithreaded download active" : L"Multithreaded download disabled");
        },
        cancel);
    check_cancelled(cancel);

    failure_status = OperationStatus::PackageVerifyFailed;
    fs::path zip_payload = select_zip_payload(payload_files);
    fs::path staging = unique_staging_dir(cfg);
    std::error_code cleanup_ec;
    try {
        progress(62, L"Staging: extracting ZIP before format");
        failure_status = OperationStatus::PackageExtractFailed;
        std::wstring password = decrypt_legacy_package_password(package.password);
        ZipExtractResult extract = extract_zip_stored_entries(zip_payload,
            staging,
            password,
            cancel,
            package_extraction_quota(package),
            package.verify_items.size());

        check_cancelled(cancel);
        progress(75, L"Staging: VerifyData before format");
        failure_status = OperationStatus::PackageVerifyFailed;
        verify_package_output(staging, package);

        PreparePayloadContext context;
        context.zip_payload = std::move(zip_payload);
        context.staging_dir = std::move(staging);
        context.extract = extract;
        context.payload_count = payload_files.size();
        return context;
    } catch (...) {
        fs::remove_all(staging, cleanup_ec);
        throw;
    }
}

bool try_log_operation_success(const Config& cfg,
    const std::wstring& brand,
    const std::wstring& part_number,
    const std::wstring& file_name,
    const std::chrono::system_clock::time_point& started) noexcept {
    try {
        return log_operation_report(cfg,
            brand,
            part_number,
            file_name,
            started,
            std::chrono::system_clock::now(),
            OperationStatus::Success);
    } catch (...) {
        return false;
    }
}

void log_prepare_failure(const Config& cfg,
    const Brand& brand,
    const std::wstring& report_part,
    const std::wstring& report_file,
    const std::chrono::system_clock::time_point& started,
    OperationStatus failure_status,
    const std::exception& ex) {
    OperationStatus status = classify_operation_failure(failure_status, ex.what());
    try {
        log_error_entry(cfg, status, exception_to_wide(ex));
        log_operation_report(cfg, brand.name, report_part, report_file, started, std::chrono::system_clock::now(), status);
    } catch (...) {
    }
}

PrepareResult run_prepare_to_folder_workflow(const Brand& brand,
    const std::wstring& part_number,
    const std::wstring& model_id,
    const std::wstring& destination,
    bool include_hidden,
    const ProgressCallback& progress,
    const CancellationCallback& cancel = {}) {
    Config cfg = load_config();
    auto started = std::chrono::system_clock::now();
    OperationStatus failure_status = OperationStatus::MetaFileError;
    std::wstring report_part = trim_w(part_number);
    std::wstring report_file;

    try {
        check_cancelled(cancel);
        PreparePackageContext package_context = resolve_prepare_package(cfg, brand, part_number, model_id, include_hidden, progress, cancel);
        check_cancelled(cancel);
        report_part = package_context.resolution.resolved_part.part_number;
        report_file = package_context.report_file;
        failure_status = OperationStatus::PackageVerifyFailed;
        validate_package_info(package_context.package, package_context.package_xml);
        PreparePayloadContext payload_context = download_extract_and_verify_payload(cfg,
            brand,
            package_context.package,
            destination,
            failure_status,
            progress,
            cancel);
        check_cancelled(cancel);

        PrepareResult result;
        result.extracted_to_folder = true;
        result.success_log_failed = !try_log_operation_success(cfg, brand.name, report_part, report_file, started);
        return result;
    } catch (const std::exception& ex) {
        log_prepare_failure(cfg, brand, report_part, report_file, started, failure_status, ex);
        throw;
    }
}

PrepareResult run_prepare_to_drive_workflow(const Brand& brand,
    const std::wstring& part_number,
    const std::wstring& model_id,
    const std::wstring& destination,
    bool include_hidden,
    const ProgressCallback& progress,
    const CancellationCallback& cancel = {}) {
    Config cfg = load_config();
    auto started = std::chrono::system_clock::now();
    OperationStatus failure_status = OperationStatus::MetaFileError;
    std::wstring report_part = trim_w(part_number);
    std::wstring report_file;
    fs::path staging_to_cleanup;

    try {
        check_cancelled(cancel);
        PreparePackageContext package_context = resolve_prepare_package(cfg, brand, part_number, model_id, include_hidden, progress, cancel);
        check_cancelled(cancel);
        report_part = package_context.resolution.resolved_part.part_number;
        report_file = package_context.report_file;
        failure_status = OperationStatus::PackageVerifyFailed;
        validate_package_info(package_context.package, package_context.package_xml);

        failure_status = OperationStatus::NoRecommendedCapacity;
        if (!is_drive_root_path(destination)) {
            throw AppError("Drive workflow requires a drive root destination");
        }
        validate_removable_destination(destination, package_context.package);
        reject_temp_directory_on_destination_volume(cfg, destination);

        progress(30, L"Preparing: checking read speed");
        check_cancelled(cancel);
        failure_status = OperationStatus::SpeedNotSuffient;
        double read_speed = check_removable_read_speed(destination, cancel);
        if (read_speed < 6.0) {
            throw AppError("SD card read speed is below required 6 MB/s: " + std::to_string(read_speed) + " MB/s");
        }

        PreparePayloadContext payload_context = download_and_stage_payload(cfg,
            brand,
            package_context.package,
            failure_status,
            progress,
            cancel);
        staging_to_cleanup = payload_context.staging_dir;

        progress(80, L"Preparing: revalidating removable drive");
        check_cancelled(cancel);
        failure_status = OperationStatus::NoRecommendedCapacity;
        validate_removable_destination(destination, package_context.package);

        progress(82, L"Preparing: formatting removable drive");
        check_cancelled(cancel);
        failure_status = OperationStatus::SdCardFormatFailed;
        format_removable_drive(destination, package_context.package, cancel);
        check_cancelled(cancel);

        progress(86, L"Commit: copying verified staging to removable drive");
        failure_status = OperationStatus::PackageExtractFailed;
        ensure_free_space_for_path(destination, package_extraction_quota(package_context.package));
        copy_staged_tree_to_destination(payload_context.staging_dir,
            destination,
            [&progress](const std::wstring& text) {
                progress(90, text);
            },
            cancel,
            [&](const std::vector<fs::path>& backup_files) {
                check_cancelled(cancel);
                progress(95, L"Validation: VerifyData on removable drive");
                failure_status = OperationStatus::PackageVerifyFailed;
                verify_package_output(destination, package_context.package, backup_files);
                check_cancelled(cancel);
            });

        std::error_code cleanup_ec;
        fs::remove_all(payload_context.staging_dir, cleanup_ec);
        staging_to_cleanup.clear();

        progress(96, L"Finishing: renaming volume");
        check_cancelled(cancel);
        failure_status = OperationStatus::Exception;
        bool renamed = rename_removable_drive(destination, package_context.resolution.resolved_part.part_number);
        DWORD rename_error = renamed ? ERROR_SUCCESS : GetLastError();
        progress(98, L"Finishing: ejecting removable drive");
        failure_status = OperationStatus::CardEjectFailed;
        for (int i = 0; i < 30; ++i) {
            check_cancelled(cancel);
            Sleep(100);
        }
        check_cancelled(cancel);
        eject_removable_drive(destination);

        if (!renamed) {
            failure_status = OperationStatus::Exception;
            throw AppError("Package was verified and the drive was ejected, but the volume could not be renamed: " +
                format_win32_error(rename_error));
        }

        PrepareResult result;
        result.extracted_to_folder = false;
        result.success_log_failed = !try_log_operation_success(cfg, brand.name, report_part, report_file, started);
        return result;
    } catch (const std::exception& ex) {
        if (!staging_to_cleanup.empty()) {
            std::error_code cleanup_ec;
            fs::remove_all(staging_to_cleanup, cleanup_ec);
        }
        log_prepare_failure(cfg, brand, report_part, report_file, started, failure_status, ex);
        throw;
    }
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

void set_control_text(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(hwnd, text.c_str());
}

void make_settings_label(HWND parent, const wchar_t* text, int x, int y, HFONT font) {
    HWND label = make_control(parent, L"STATIC", text, 0, 0, 0);
    MoveWindow(label, x, y + 4, 120, 22, TRUE);
    apply_font(label, font);
}

std::wstring source_summary_text(const Config& cfg) {
    if (!catalog_source_is_configured(cfg)) {
        return L"Not configured";
    }
    if (cfg.use_local_catalog) {
        return cfg.local_catalog_root.empty() ? L"Local catalog: <empty>" : L"Local catalog: " + cfg.local_catalog_root.wstring();
    }
    std::wstring network = trim_w(cfg.catalog_root);
    if (!network.empty()) {
        std::wstring summary = L"Network: " + network;
        if (looks_like_ftp_url(network)) {
            summary += L" [" + ftp_mode_to_string(cfg.ftp_mode) + L"]";
        }
        return summary;
    }
    return L"WebDAV: " + webdav_home_url(cfg);
}

bool source_use_local(const SourceDialogState& state) {
    return SendMessageW(state.use_local_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

bool looks_like_unc_catalog_path(std::wstring_view value) {
    return value.size() >= 3 &&
        ((value[0] == L'\\' && value[1] == L'\\') ||
            (value[0] == L'/' && value[1] == L'/'));
}

bool local_catalog_marker_exists(const fs::path& root) {
    std::error_code ec;
    fs::path marker = root / L"common" / L"Settings" / L"brands.xml";
    return fs::is_regular_file(marker, ec) && !ec;
}

void configure_local_catalog(Config& cfg, const fs::path& requested_root) {
    static constexpr wchar_t kDefaultChannel[] = L"Trade-Retail";

    cfg.use_local_catalog = true;
    cfg.local_catalog_root = requested_root;
    cfg.catalog_root.clear();

    // A selected channel directory already contains common/Settings/brands.xml.
    // A Service42-MMI root needs exactly one channel component appended later by
    // catalog_file_source_path(). Preserve a valid configured channel and fall
    // back to the donor-compatible retail channel for a newly selected root.
    if (local_catalog_marker_exists(requested_root)) {
        cfg.distribution_channel.clear();
        return;
    }

    std::wstring channel = trim_slashes(cfg.distribution_channel);
    cfg.distribution_channel.clear();
    if (!channel.empty() && local_catalog_marker_exists(requested_root / channel)) {
        cfg.distribution_channel = channel;
        return;
    }
    if (local_catalog_marker_exists(requested_root / kDefaultChannel)) {
        cfg.distribution_channel = kDefaultChannel;
    }
}

void add_ftp_mode_item(HWND combo, const wchar_t* text, FtpMode mode) {
    LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(static_cast<int>(mode)));
}

void fill_ftp_mode_combo(HWND combo, FtpMode selected_mode) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    add_ftp_mode_item(combo, L"Auto: passive, then active", FtpMode::Auto);
    add_ftp_mode_item(combo, L"Passive (PASV)", FtpMode::Passive);
    add_ftp_mode_item(combo, L"Active (PORT)", FtpMode::Active);
    for (int i = 0; i < 3; ++i) {
        LRESULT data = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0);
        if (data != CB_ERR && static_cast<FtpMode>(static_cast<int>(data)) == selected_mode) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            return;
        }
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

FtpMode selected_ftp_mode(HWND combo) {
    LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) {
        return FtpMode::Auto;
    }
    LRESULT data = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
    if (data == CB_ERR) {
        return FtpMode::Auto;
    }
    return static_cast<FtpMode>(static_cast<int>(data));
}

void update_source_controls(SourceDialogState& state) {
    bool use_local = source_use_local(state);
    EnableWindow(state.local_edit, use_local);
    EnableWindow(state.local_add_button, TRUE);
    EnableWindow(state.network_edit, !use_local);
    EnableWindow(state.ftp_mode_combo, !use_local);
    EnableWindow(state.username_edit, !use_local);
    EnableWindow(state.password_edit, !use_local);
    EnableWindow(state.test_button, TRUE);
}

void fill_source_dialog(SourceDialogState& state) {
    Config cfg = load_config();
    SendMessageW(state.use_local_check, BM_SETCHECK, cfg.use_local_catalog ? BST_CHECKED : BST_UNCHECKED, 0);
    set_control_text(state.local_edit, cfg.local_catalog_root.wstring());
    std::wstring network = trim_w(cfg.catalog_root);
    state.loaded_legacy_webdav = !cfg.use_local_catalog &&
        (cfg.source_transport == SourceTransport::WebDav ||
            (cfg.source_transport == SourceTransport::Auto && network.empty())) &&
        legacy_webdav_source_is_configured(cfg);
    if (state.loaded_legacy_webdav) {
        network = webdav_home_url(cfg);
    }
    state.initial_network = network;
    set_control_text(state.network_edit, network);
    fill_ftp_mode_combo(state.ftp_mode_combo, cfg.ftp_mode);
    set_control_text(state.username_edit, cfg.username);
    try {
        set_control_text(state.password_edit, unprotect_password(cfg.password_dpapi));
    } catch (...) {
        set_control_text(state.password_edit, L"");
    }
    update_source_controls(state);
}

Config source_config_from_dialog(SourceDialogState& state) {
    Config cfg = load_config();
    cfg.source_transport = SourceTransport::Auto;
    bool use_local = source_use_local(state);
    std::wstring local = trim_w(get_window_text(state.local_edit));
    std::wstring network = trim_w(get_window_text(state.network_edit));
    if (!use_local && looks_like_unc_catalog_path(network)) {
        use_local = true;
        local = network;
    }
    cfg.use_local_catalog = use_local;
    if (!cfg.use_local_catalog && !state.loaded_legacy_webdav &&
        network == state.initial_network && !trim_w(cfg.catalog_root).empty() &&
        !trim_slashes(cfg.distribution_channel).empty()) {
        network = catalog_network_url_full(cfg, L"");
    }
    if (cfg.use_local_catalog) {
        if (local.empty()) {
            throw AppError("Local catalog directory is empty");
        }
        configure_local_catalog(cfg, fs::path(local));
    } else {
        if (!looks_like_url(network)) {
            throw AppError("Network catalog source requires an http://, https://, or ftp:// URL");
        }
        cfg.catalog_root = network;
        cfg.local_catalog_root.clear();
    }
    cfg.server.clear();
    cfg.top_level_collection.clear();
    if (!cfg.use_local_catalog) {
        cfg.distribution_channel.clear();
    }
    cfg.ftp_mode = selected_ftp_mode(state.ftp_mode_combo);
    if (cfg.use_local_catalog) {
        cfg.username.clear();
        cfg.password_dpapi.clear();
    } else {
        cfg.username = trim_w(get_window_text(state.username_edit));
        std::wstring password = get_window_text(state.password_edit);
        cfg.password_dpapi = password.empty() ? std::string{} : protect_password(password);
        validate_config_credentials_complete(cfg);
        if (config_has_credentials(cfg) && !credentials_target_an_https_source(cfg)) {
            throw AppError("Credentials require an HTTPS catalog source");
        }
    }
    return cfg;
}

void save_source_dialog(SourceDialogState& state) {
    Config cfg = source_config_from_dialog(state);
    save_config(cfg);
    state.saved = true;
}

void test_source_dialog(HWND window, SourceDialogState& state) {
    Config cfg = source_config_from_dialog(state);
    DWORD status = probe_catalog_source(cfg, L"common/Settings/brands.xml");
    std::wstring message = L"Connection OK: " + std::to_wstring(status) +
        L"\r\n" + catalog_location_description(cfg, L"common/Settings/brands.xml");
    MessageBoxW(window, message.c_str(), L"Source", MB_ICONINFORMATION);
}

enum class FolderPickerFailureStage {
    None,
    CreateDialog,
    ShowDialog,
    GetResult,
    GetDisplayName
};

struct FolderPickerResult {
    std::optional<fs::path> path;
    FolderPickerFailureStage failure_stage = FolderPickerFailureStage::None;
    HRESULT failure_code = S_OK;
};

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

LRESULT handle_static_control_color(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    wchar_t class_name[16]{};
    GetClassNameW(reinterpret_cast<HWND>(lparam), class_name, 16);
    if (wcscmp(class_name, L"Static") != 0 && wcscmp(class_name, L"Button") != 0) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    HDC dc = reinterpret_cast<HDC>(wparam);
    SetBkMode(dc, TRANSPARENT);
    return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
}

void choose_source_local_catalog(HWND window, SourceDialogState& state) {
    FolderPickerResult result = pick_folder_path(
        window,
        L"Select local catalog",
        trim_w(get_window_text(state.local_edit)));
    if (result.failure_stage != FolderPickerFailureStage::None) {
        throw_folder_picker_failure(result);
    }
    if (!result.path) {
        return;
    }

    set_control_text(state.local_edit, result.path->c_str());
    SendMessageW(state.use_local_check, BM_SETCHECK, BST_CHECKED, 0);
    update_source_controls(state);
}

LRESULT CALLBACK source_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        auto* state = reinterpret_cast<SourceDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        const int margin = 14;
        const int label_w = 120;
        const int edit_x = margin + label_w;
        const int edit_w = 420;
        const int local_add_w = 82;
        const int gap = 8;
        int y = margin;

        state->use_local_check = make_control(window, L"BUTTON", L"Use folder catalog (local or network/UNC)", BS_AUTOCHECKBOX, 0, IDC_SOURCE_USE_LOCAL);
        MoveWindow(state->use_local_check, edit_x, y, edit_w, 24, TRUE);
        apply_font(state->use_local_check, state->font);
        y += 34;

        make_settings_label(window, L"Folder", margin, y, state->font);
        state->local_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SOURCE_LOCAL);
        state->local_add_button = make_control(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON, 0, IDC_SOURCE_LOCAL_ADD);
        SendMessageW(state->local_edit, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"Путь к папке Service42-MMI"));
        MoveWindow(state->local_edit, edit_x, y, edit_w - local_add_w - gap, 24, TRUE);
        MoveWindow(state->local_add_button, edit_x + edit_w - local_add_w, y, local_add_w, 24, TRUE);
        apply_font(state->local_edit, state->font);
        apply_font(state->local_add_button, state->font);
        y += 34;

        make_settings_label(window, L"URL", margin, y, state->font);
        state->network_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SOURCE_NETWORK);
        MoveWindow(state->network_edit, edit_x, y, edit_w, 24, TRUE);
        apply_font(state->network_edit, state->font);
        y += 34;

        make_settings_label(window, L"FTP mode", margin, y, state->font);
        state->ftp_mode_combo = make_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0, IDC_SOURCE_FTP_MODE);
        MoveWindow(state->ftp_mode_combo, edit_x, y, edit_w, 120, TRUE);
        apply_font(state->ftp_mode_combo, state->font);
        y += 34;

        make_settings_label(window, L"Username", margin, y, state->font);
        state->username_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SOURCE_USERNAME);
        MoveWindow(state->username_edit, edit_x, y, edit_w, 24, TRUE);
        apply_font(state->username_edit, state->font);
        y += 34;

        make_settings_label(window, L"Password", margin, y, state->font);
        state->password_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD, WS_EX_CLIENTEDGE, IDC_SOURCE_PASSWORD);
        MoveWindow(state->password_edit, edit_x, y, edit_w, 24, TRUE);
        apply_font(state->password_edit, state->font);
        y += 42;

        state->test_button = make_control(window, L"BUTTON", L"Test", BS_PUSHBUTTON, 0, IDC_SOURCE_TEST);
        HWND ok = make_control(window, L"BUTTON", L"OK", BS_DEFPUSHBUTTON, 0, IDC_SOURCE_OK);
        HWND cancel = make_control(window, L"BUTTON", L"Cancel", BS_PUSHBUTTON, 0, IDC_SOURCE_CANCEL);
        MoveWindow(state->test_button, edit_x, y, 82, 28, TRUE);
        MoveWindow(ok, edit_x + edit_w - 176, y, 82, 28, TRUE);
        MoveWindow(cancel, edit_x + edit_w - 88, y, 88, 28, TRUE);
        apply_font(state->test_button, state->font);
        apply_font(ok, state->font);
        apply_font(cancel, state->font);

        fill_source_dialog(*state);
        return 0;
    }
    case WM_COMMAND: {
        auto* state = reinterpret_cast<SourceDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        int id = LOWORD(wparam);
        if (id == IDC_SOURCE_OK) {
            try {
                save_source_dialog(*state);
                DestroyWindow(window);
            } catch (const std::exception& ex) {
                MessageBoxW(window, exception_to_wide(ex).c_str(), L"Source", MB_ICONERROR);
            }
            return 0;
        }
        if (id == IDC_SOURCE_CANCEL || id == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (id == IDC_SOURCE_TEST && HIWORD(wparam) == BN_CLICKED) {
            try {
                HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                test_source_dialog(window, *state);
                SetCursor(previous);
            } catch (const std::exception& ex) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                MessageBoxW(window, exception_to_wide(ex).c_str(), L"Source", MB_ICONERROR);
            }
            return 0;
        }
        if (id == IDC_SOURCE_LOCAL_ADD && HIWORD(wparam) == BN_CLICKED) {
            try {
                choose_source_local_catalog(window, *state);
            } catch (const std::exception& ex) {
                MessageBoxW(window, exception_to_wide(ex).c_str(), L"Source", MB_ICONERROR);
            }
            return 0;
        }
        if (id == IDC_SOURCE_USE_LOCAL && HIWORD(wparam) == BN_CLICKED) {
            update_source_controls(*state);
            return 0;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        return handle_static_control_color(window, message, wparam, lparam);
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

class DisabledOwnerGuard {
public:
    explicit DisabledOwnerGuard(HWND owner) : owner_(owner) {
        EnableWindow(owner_, FALSE);
    }

    DisabledOwnerGuard(const DisabledOwnerGuard&) = delete;
    DisabledOwnerGuard& operator=(const DisabledOwnerGuard&) = delete;

    ~DisabledOwnerGuard() {
        EnableWindow(owner_, TRUE);
        SetForegroundWindow(owner_);
    }

private:
    HWND owner_ = nullptr;
};

void run_modal_window_loop(HWND dialog) {
    MSG msg{};
    while (IsWindow(dialog)) {
        BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result == -1) {
            throw AppError("GetMessage failed in modal window: " + format_win32_error(GetLastError()));
        }
        if (result == 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            return;
        }
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

bool show_source_dialog(HWND owner) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = source_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kSourceClass;
    RegisterClassExW(&wc);

    SourceDialogState state;
    state.owner = owner;
    DisabledOwnerGuard owner_guard(owner);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME,
        kSourceClass,
        L"Source",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        600,
        324,
        owner,
        nullptr,
        instance,
        &state);
    if (!dialog) {
        throw AppError("CreateWindowEx failed for source dialog");
    }
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    run_modal_window_loop(dialog);
    return state.saved;
}

bool settings_logging_enabled(const SettingsDialogState& state) {
    return SendMessageW(state.logging_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void update_settings_controls(SettingsDialogState& state) {
    bool enabled = settings_logging_enabled(state);
    EnableWindow(state.log_edit, enabled);
    EnableWindow(state.log_add_button, enabled);
}

void choose_settings_folder(HWND window, HWND target_edit, const wchar_t* title) {
    FolderPickerResult result = pick_folder_path(
        window,
        title,
        trim_w(get_window_text(target_edit)));
    if (result.failure_stage != FolderPickerFailureStage::None) {
        throw_folder_picker_failure(result);
    }
    if (result.path) {
        set_control_text(target_edit, result.path->c_str());
    }
}

void fill_settings_dialog(SettingsDialogState& state) {
    Config cfg = load_config();
    set_control_text(state.source_static, source_summary_text(cfg));
    set_control_text(state.metadata_edit, cfg.metadata_dir.wstring());
    set_control_text(state.temp_edit, cfg.temp_dir.wstring());
    SendMessageW(state.logging_check, BM_SETCHECK, cfg.logging_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    set_control_text(state.log_edit, log_dir(cfg).wstring());
    update_settings_controls(state);
}

void save_settings_dialog(SettingsDialogState& state) {
    Config cfg = load_config();
    std::wstring metadata = trim_w(get_window_text(state.metadata_edit));
    std::wstring temp = trim_w(get_window_text(state.temp_edit));
    std::wstring log = trim_w(get_window_text(state.log_edit));
    if (!metadata.empty()) {
        cfg.metadata_dir = fs::path(metadata);
    }
    if (!temp.empty()) {
        cfg.temp_dir = fs::path(temp);
    }
    cfg.logging_enabled = settings_logging_enabled(state);
    cfg.log_dir = log.empty() ? executable_dir() / L"Logs" : fs::path(log);
    save_config(cfg);
    fs::create_directories(cfg.metadata_dir);
    fs::create_directories(cfg.temp_dir);
    if (cfg.logging_enabled) {
        fs::create_directories(log_dir(cfg));
    }
    state.saved = true;
}

LRESULT CALLBACK settings_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        auto* state = reinterpret_cast<SettingsDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        const int margin = 16;
        const int label_w = 126;
        const int edit_x = margin + label_w;
        const int content_w = 638;
        const int browse_w = 96;
        const int gap = 8;
        const int edit_w = content_w - label_w - browse_w - gap;
        int y = margin;

        auto add_heading = [&](const wchar_t* text) {
            HWND heading = make_control(window, L"STATIC", text, SS_LEFT, 0, 0);
            MoveWindow(heading, margin, y, content_w, 20, TRUE);
            apply_font(heading, state->font);
            y += 24;
        };

        add_heading(L"SOURCE");
        state->source_static = make_control(window, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, WS_EX_CLIENTEDGE, IDC_SETTINGS_SOURCE_TEXT);
        HWND add_source = make_control(window, L"BUTTON", L"Change...", BS_PUSHBUTTON, 0, IDC_SETTINGS_SOURCE_ADD);
        MoveWindow(state->source_static, margin, y, content_w - browse_w - gap, 26, TRUE);
        MoveWindow(add_source, margin + content_w - browse_w, y, browse_w, 26, TRUE);
        apply_font(state->source_static, state->font);
        apply_font(add_source, state->font);
        y += 40;

        add_heading(L"STORAGE");
        make_settings_label(window, L"Metadata folder", margin, y, state->font);
        state->metadata_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SETTINGS_METADATA);
        HWND metadata_browse = make_control(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON, 0, IDC_SETTINGS_METADATA_BROWSE);
        MoveWindow(state->metadata_edit, edit_x, y, edit_w, 24, TRUE);
        MoveWindow(metadata_browse, edit_x + edit_w + gap, y, browse_w, 24, TRUE);
        apply_font(state->metadata_edit, state->font);
        apply_font(metadata_browse, state->font);
        y += 34;

        make_settings_label(window, L"Temporary folder", margin, y, state->font);
        state->temp_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SETTINGS_TEMP);
        HWND temp_browse = make_control(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON, 0, IDC_SETTINGS_TEMP_BROWSE);
        MoveWindow(state->temp_edit, edit_x, y, edit_w, 24, TRUE);
        MoveWindow(temp_browse, edit_x + edit_w + gap, y, browse_w, 24, TRUE);
        apply_font(state->temp_edit, state->font);
        apply_font(temp_browse, state->font);
        y += 42;

        add_heading(L"LOGGING");
        state->logging_check = make_control(window, L"BUTTON", L"Write operation logs to files", BS_AUTOCHECKBOX, 0, IDC_SETTINGS_LOGGING);
        MoveWindow(state->logging_check, margin, y, content_w, 24, TRUE);
        apply_font(state->logging_check, state->font);
        y += 30;

        make_settings_label(window, L"Log folder", margin, y, state->font);
        state->log_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SETTINGS_LOG);
        state->log_add_button = make_control(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON, 0, IDC_SETTINGS_LOG_ADD);
        MoveWindow(state->log_edit, edit_x, y, edit_w, 24, TRUE);
        MoveWindow(state->log_add_button, edit_x + edit_w + gap, y, browse_w, 24, TRUE);
        apply_font(state->log_edit, state->font);
        apply_font(state->log_add_button, state->font);
        y += 34;

        y += 8;

        HWND ok = make_control(window, L"BUTTON", L"Save", BS_DEFPUSHBUTTON, 0, IDC_SETTINGS_OK);
        HWND cancel = make_control(window, L"BUTTON", L"Cancel", BS_PUSHBUTTON, 0, IDC_SETTINGS_CANCEL);
        MoveWindow(ok, margin + content_w - 192, y, 92, 30, TRUE);
        MoveWindow(cancel, margin + content_w - 92, y, 92, 30, TRUE);
        apply_font(ok, state->font);
        apply_font(cancel, state->font);

        fill_settings_dialog(*state);
        return 0;
    }
    case WM_COMMAND: {
        auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        int id = LOWORD(wparam);
        if (id == IDC_SETTINGS_OK) {
            try {
                save_settings_dialog(*state);
                DestroyWindow(window);
            } catch (const std::exception& ex) {
                MessageBoxW(window, exception_to_wide(ex).c_str(), L"Settings", MB_ICONERROR);
            }
            return 0;
        }
        if (id == IDC_SETTINGS_CANCEL || id == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (id == IDC_SETTINGS_SOURCE_ADD && HIWORD(wparam) == BN_CLICKED) {
            if (show_source_dialog(window)) {
                fill_settings_dialog(*state);
                state->saved = true;
            }
            return 0;
        }
        if (id == IDC_SETTINGS_LOGGING && HIWORD(wparam) == BN_CLICKED) {
            update_settings_controls(*state);
            return 0;
        }
        if (id == IDC_SETTINGS_LOG_ADD && HIWORD(wparam) == BN_CLICKED) {
            try {
                choose_settings_folder(window, state->log_edit, L"Select log folder");
            } catch (const std::exception& ex) {
                MessageBoxW(window, exception_to_wide(ex).c_str(), L"Settings", MB_ICONERROR);
            }
            return 0;
        }
        if ((id == IDC_SETTINGS_METADATA_BROWSE || id == IDC_SETTINGS_TEMP_BROWSE) &&
            HIWORD(wparam) == BN_CLICKED) {
            try {
                HWND target = id == IDC_SETTINGS_METADATA_BROWSE ? state->metadata_edit : state->temp_edit;
                const wchar_t* title = id == IDC_SETTINGS_METADATA_BROWSE
                    ? L"Select metadata folder"
                    : L"Select temporary folder";
                choose_settings_folder(window, target, title);
            } catch (const std::exception& ex) {
                MessageBoxW(window, exception_to_wide(ex).c_str(), L"Settings", MB_ICONERROR);
            }
            return 0;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        return handle_static_control_color(window, message, wparam, lparam);
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool show_settings_dialog(HWND owner) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = settings_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kSettingsClass;
    RegisterClassExW(&wc);

    SettingsDialogState state;
    state.owner = owner;
    DisabledOwnerGuard owner_guard(owner);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME,
        kSettingsClass,
        L"Settings",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        700,
        424,
        owner,
        nullptr,
        instance,
        &state);
    if (!dialog) {
        throw AppError("CreateWindowEx failed for settings dialog");
    }
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    run_modal_window_loop(dialog);
    return state.saved;
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

std::optional<DriveInfoNative> selected_drive(const GuiState& state) {
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
                post_error(window, request_id, kind, exception_to_wide(ex));
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
        ProgressCallback progress = [window, request_id, &cancel](int value, const std::wstring& text) {
            check_cancelled(cancel);
            post_progress(window, request_id, WorkerKind::Prepare, value, text);
        };
        check_cancelled(cancel);
        PrepareResult result = extract_to_folder
            ? run_prepare_to_folder_workflow(brand, part_number, model_id, destination, include_hidden, progress, cancel)
            : run_prepare_to_drive_workflow(brand, part_number, model_id, destination, include_hidden, progress, cancel);
        result.request_id = request_id;
        auto* payload = new PrepareResult(std::move(result));
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
            MessageBoxW(window, exception_to_wide(ex).c_str(), L"MMIDownloader", MB_ICONERROR);
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
            std::wstring message_text = exception_to_wide(ex);
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
        std::unique_ptr<PrepareResult> result(take_payload<PrepareResult>(lparam));
        GuiState* state = state_from(window);
        if (state && result && result->request_id == state->active_request_id) {
            state->active_request_id = 0;
            if (state->close_requested) {
                state->cancel_requested.reset();
                return 0;
            }
            SendMessageW(state->progress, PBM_SETPOS, 100, 0);
            append_log(*state,
                result->extracted_to_folder
                    ? L"Completed - files are ready in the selected folder"
                    : L"Completed - the SD card is ready");
            if (result->success_log_failed) {
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
        MessageBoxW(nullptr, exception_to_wide(ex).c_str(), L"MMIDownloader", MB_ICONERROR | MB_OK);
        return 1;
    } catch (...) {
        MessageBoxW(nullptr, L"Unknown startup error", L"MMIDownloader", MB_ICONERROR | MB_OK);
        return 1;
    }
}
