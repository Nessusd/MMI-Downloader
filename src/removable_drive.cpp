#include "removable_drive.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>
#include <winioctl.h>

namespace fs = std::filesystem;

namespace mmid {

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
    const std::wstring root = normalize_drive_root(value);
    return root.size() == 3 &&
        ((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')) &&
        root[1] == L':' &&
        root[2] == L'\\';
}

std::optional<RemovableDriveInfo> removable_drive_info(const std::wstring& root_value) {
    const std::wstring root = normalize_drive_root(root_value);
    if (!is_drive_root_path(root) || GetDriveTypeW(root.c_str()) != DRIVE_REMOVABLE) {
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

    RemovableDriveInfo info;
    info.root = root;
    info.label = volume_name;
    info.total_bytes = total_bytes.QuadPart;
    info.free_bytes = total_free_bytes.QuadPart;
    return info;
}

std::vector<RemovableDriveInfo> enumerate_removable_drives() {
    const DWORD needed = GetLogicalDriveStringsW(0, nullptr);
    if (needed == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1, L'\0');
    const DWORD written = GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (written == 0) {
        return {};
    }

    std::vector<RemovableDriveInfo> drives;
    const wchar_t* current = buffer.data();
    while (*current) {
        if (auto info = removable_drive_info(current)) {
            drives.push_back(std::move(*info));
        }
        current += wcslen(current) + 1;
    }
    return drives;
}

namespace {

std::wstring capacity_text(uint64_t bytes) {
    const double gb = static_cast<double>(bytes) / 1024.0 / 1024.0 / 1024.0;
    std::wostringstream output;
    output << std::fixed << std::setprecision(gb < 10.0 ? 1 : 0) << gb << L" GB";
    return output.str();
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

std::wstring filesystem_for_drive(const PackageInfo& package, const RemovableDriveInfo& drive) {
    std::wstring recommended = trim_w(package.sd_format);
    if (recommended.empty()) {
        recommended = L"FAT32";
    }

    std::wstring filesystem;
    if (iequals_w(recommended, L"FAT")) {
        filesystem = L"FAT";
    } else if (iequals_w(recommended, L"FAT32")) {
        filesystem = L"FAT32";
    } else if (iequals_w(recommended, L"exFAT")) {
        filesystem = L"exFAT";
    } else if (iequals_w(recommended, L"NTFS")) {
        filesystem = L"NTFS";
    } else {
        throw AppError("Unsupported SD filesystem in package metadata: " + wide_to_utf8(recommended));
    }

    if (drive.total_bytes > 34359738368ull && filesystem == L"FAT32") {
        return L"exFAT";
    }
    return filesystem;
}

DWORD run_hidden_process(
    const std::wstring& executable,
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
        throw AppError("CreateProcess failed for " + wide_to_utf8(executable) + ": " +
            format_win32_error(GetLastError()));
    }

    auto terminate_process_and_wait = [&process]() {
        if (!process.hProcess || WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            return;
        }

        (void)TerminateProcess(process.hProcess, ERROR_CANCELLED);
        for (;;) {
            if (WaitForSingleObject(process.hProcess, 1000) == WAIT_OBJECT_0) {
                return;
            }

            DWORD exit_code = STILL_ACTIVE;
            if (GetExitCodeProcess(process.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
                return;
            }
            (void)TerminateProcess(process.hProcess, ERROR_CANCELLED);
        }
    };

    const uint64_t started = GetTickCount64();
    for (;;) {
        const DWORD wait_result = WaitForSingleObject(process.hProcess, 200);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (wait_result == WAIT_FAILED) {
            const DWORD error = GetLastError();
            terminate_process_and_wait();
            throw AppError("WaitForSingleObject failed for " + wide_to_utf8(executable) + ": " +
                format_win32_error(error));
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
        throw AppError("GetExitCodeProcess failed for " + wide_to_utf8(executable) + ": " +
            format_win32_error(GetLastError()));
    }
    return exit_code;
}

void wait_for_ready_removable_drive(const std::wstring& root, const CancellationCallback& cancel) {
    for (int attempt = 0; attempt < 60; ++attempt) {
        check_cancelled(cancel);
        if (removable_drive_info(root)) {
            return;
        }
        Sleep(500);
    }
    throw AppError("Removable drive did not become ready after format: " + wide_to_utf8(root));
}

class WinHandle final {
public:
    explicit WinHandle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;

    ~WinHandle() {
        close();
    }

    HANDLE get() const noexcept {
        return handle_;
    }

    explicit operator bool() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    void close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_;
};

fs::path create_speed_test_source_file(uint64_t size_bytes, const CancellationCallback& cancel) {
    const fs::path temporary_directory = fs::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
        const fs::path path = temporary_directory / fs::path(L"mmidownloader-speed-" +
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
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                continue;
            }
            throw AppError("Could not create speed test source file: " + printable_path(path) +
                " (" + format_win32_error(error) + ")");
        }

        try {
            std::vector<uint8_t> buffer(1024 * 1024);
            for (size_t index = 0; index < buffer.size(); ++index) {
                buffer[index] = static_cast<uint8_t>(index);
            }

            uint64_t written_total = 0;
            while (written_total < size_bytes) {
                check_cancelled(cancel);
                const DWORD to_write = static_cast<DWORD>(
                    std::min<uint64_t>(buffer.size(), size_bytes - written_total));
                DWORD written = 0;
                if (!WriteFile(file.get(), buffer.data(), to_write, &written, nullptr) || written != to_write) {
                    throw AppError("Could not write speed test source file: " +
                        format_win32_error(GetLastError()));
                }
                written_total += written;
            }
            if (!FlushFileBuffers(file.get())) {
                throw AppError("Could not flush speed test source file: " +
                    format_win32_error(GetLastError()));
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
    std::error_code error;
    (void)fs::remove(path, error);
    if (error) {
        throw AppError(std::string("Could not remove ") + description + ": " +
            printable_path(path) + " (" + error.message() + ")");
    }

    const bool still_exists = fs::exists(path, error);
    if (error) {
        throw AppError(std::string("Could not confirm removal of ") + description + ": " +
            printable_path(path) + " (" + error.message() + ")");
    }
    if (still_exists) {
        throw AppError(std::string("Removal of ") + description + " was not completed: " +
            printable_path(path));
    }
}

void remove_speed_test_file_best_effort(const fs::path& path) noexcept {
    if (path.empty()) {
        return;
    }
    std::error_code error;
    (void)fs::remove(path, error);
}

bool device_io_control(HANDLE handle, DWORD code, void* input = nullptr, DWORD input_size = 0) {
    DWORD returned = 0;
    return DeviceIoControl(handle, code, input, input_size, nullptr, 0, &returned, nullptr) != FALSE;
}

bool wait_for_drive_removed(const std::wstring& root) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (!removable_drive_info(root)) {
            return true;
        }
        Sleep(500);
    }
    return !removable_drive_info(root).has_value();
}

} // namespace

void WindowsRemovableDriveOperations::ensure_temporary_directory_is_safe(
    const fs::path& temporary_directory,
    const fs::path& destination) const {
    const std::wstring temporary_volume = volume_root_for_path(temporary_directory, "temporary directory");
    const std::wstring destination_volume = volume_root_for_path(destination, "destination drive");
    if (iequals_w(temporary_volume, destination_volume)) {
        throw AppError("Temporary directory must not be located on the selected SD card because the card will be formatted");
    }
}

void WindowsRemovableDriveOperations::validate_destination(
    const std::wstring& destination,
    const PackageInfo& package) const {
    if (!is_drive_root_path(destination)) {
        return;
    }

    const auto info = removable_drive_info(destination);
    if (!info) {
        throw AppError("Selected destination is not a ready removable drive: " + wide_to_utf8(destination));
    }
    if (package.recommended_capacity_gb <= 0) {
        return;
    }

    const uint64_t recommended =
        static_cast<uint64_t>(package.recommended_capacity_gb) * 1024ull * 1024ull * 1024ull;
    const uint64_t threshold = recommended - recommended / 10;
    if (info->total_bytes <= threshold) {
        throw AppError("Removable drive capacity is below package recommendation: " +
            wide_to_utf8(info->root) + " total=" + wide_to_utf8(capacity_text(info->total_bytes)) +
            ", required=" + std::to_string(package.recommended_capacity_gb) + " GB");
    }
}

double WindowsRemovableDriveOperations::measure_read_speed(
    const std::wstring& root_value,
    const CancellationCallback& cancel) const {
    const std::wstring root = normalize_drive_root(root_value);
    const auto drive = removable_drive_info(root);
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

        const auto start = std::chrono::steady_clock::now();
        const uint64_t read_bytes = read_file_no_buffering(target, cancel);
        const auto finish = std::chrono::steady_clock::now();
        const std::chrono::duration<double> elapsed = finish - start;
        if (read_bytes != test_size || elapsed.count() <= 0.0) {
            throw AppError("Speed check read size mismatch");
        }

        const double speed = std::round(
            (static_cast<double>(read_bytes) / 1024.0 / 1024.0 / elapsed.count()) * 100.0) / 100.0;
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

void WindowsRemovableDriveOperations::format(
    const std::wstring& root_value,
    const PackageInfo& package,
    const CancellationCallback& cancel) const {
    const std::wstring root = normalize_drive_root(root_value);
    const auto drive = removable_drive_info(root);
    if (!drive) {
        throw AppError("Cannot format non-removable or not-ready drive: " + wide_to_utf8(root));
    }

    const std::wstring filesystem = filesystem_for_drive(package, *drive);
    wchar_t system_directory[MAX_PATH]{};
    const UINT system_length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (system_length == 0 || system_length >= MAX_PATH) {
        throw AppError("GetSystemDirectory failed: " + format_win32_error(GetLastError()));
    }

    const fs::path format_path = fs::path(system_directory) / L"format.com";
    std::wstring arguments = L"/FS:" + filesystem + L" /V:SDCREATOR /Q /Y ";
    arguments += towupper(root[0]);
    arguments += L":";

    constexpr uint64_t format_timeout_ms = 15ull * 60ull * 1000ull;
    const DWORD exit_code = run_hidden_process(format_path.wstring(), arguments, cancel, format_timeout_ms);
    if (exit_code != 0) {
        throw AppError("format.com failed for " + wide_to_utf8(root) +
            " with exit code " + std::to_string(exit_code));
    }
    wait_for_ready_removable_drive(root, cancel);
}

std::optional<std::string> WindowsRemovableDriveOperations::rename(
    const std::wstring& root_value,
    const std::wstring& part_number) const {
    const std::wstring root = normalize_drive_root(root_value);
    if (!is_drive_root_path(root)) {
        return std::string("destination is not a drive root");
    }

    const std::wstring label = part_number.substr(0, std::min<size_t>(11, part_number.size()));
    if (SetVolumeLabelW(root.c_str(), label.c_str())) {
        return std::nullopt;
    }
    return format_win32_error(GetLastError());
}

void WindowsRemovableDriveOperations::eject(
    const std::wstring& root_value,
    const CancellationCallback& cancel) const {
    for (int attempt = 0; attempt < 30; ++attempt) {
        check_cancelled(cancel);
        Sleep(100);
    }
    check_cancelled(cancel);

    const std::wstring root = normalize_drive_root(root_value);
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
        throw AppError("CreateFile failed for " + wide_to_utf8(volume_path) + ": " +
            format_win32_error(GetLastError()));
    }

    bool locked = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (device_io_control(volume.get(), FSCTL_LOCK_VOLUME)) {
            locked = true;
            break;
        }
        Sleep(500);
    }
    if (!locked) {
        throw AppError("Could not lock volume before eject: " + wide_to_utf8(root) +
            " (" + format_win32_error(GetLastError()) + ")");
    }

    if (!device_io_control(volume.get(), FSCTL_DISMOUNT_VOLUME)) {
        throw AppError("Could not dismount volume before eject: " + wide_to_utf8(root) +
            " (" + format_win32_error(GetLastError()) + ")");
    }

    PREVENT_MEDIA_REMOVAL removal{};
    removal.PreventMediaRemoval = FALSE;
    (void)device_io_control(volume.get(), IOCTL_STORAGE_MEDIA_REMOVAL, &removal, sizeof(removal));

    if (!device_io_control(volume.get(), IOCTL_STORAGE_EJECT_MEDIA)) {
        throw AppError("Could not eject removable drive: " + wide_to_utf8(root) +
            " (" + format_win32_error(GetLastError()) + ")");
    }

    volume.close();
    if (!wait_for_drive_removed(root)) {
        throw AppError("Eject command succeeded but drive is still mounted: " + wide_to_utf8(root));
    }
}

} // namespace mmid
