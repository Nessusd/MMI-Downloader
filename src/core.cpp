#include "core.h"
#include "version.h"
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <bzlib.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
__declspec(dllimport) HINTERNET WINAPI InternetOpenW(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
__declspec(dllimport) HINTERNET WINAPI InternetConnectW(HINTERNET, LPCWSTR, INTERNET_PORT, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);
__declspec(dllimport) HINTERNET WINAPI FtpOpenFileW(HINTERNET, LPCWSTR, DWORD, DWORD, DWORD_PTR);
__declspec(dllimport) BOOL WINAPI InternetReadFile(HINTERNET, LPVOID, DWORD, LPDWORD);
__declspec(dllimport) BOOL WINAPI InternetSetOptionW(HINTERNET, DWORD, LPVOID, DWORD);
__declspec(dllimport) BOOL WINAPI InternetGetLastResponseInfoW(LPDWORD, LPWSTR, LPDWORD);
__declspec(dllimport) BOOL WINAPI InternetCloseHandle(HINTERNET);
}

namespace fs = std::filesystem;

namespace mmid {

constexpr wchar_t kAppName[] = L"MMIDownloader";
constexpr char kAppVersion[] = MMID_APP_VERSION;
constexpr char kBuildNumber[] = MMID_BUILD_NUMBER;
constexpr DWORD kInternetOpenTypePreconfig = 0;
constexpr DWORD kInternetServiceFtp = 1;
constexpr DWORD kInternetFlagReload = 0x80000000;
constexpr DWORD kInternetFlagNoCacheWrite = 0x04000000;
constexpr DWORD kInternetFlagPassive = 0x08000000;
constexpr DWORD kFtpTransferTypeBinary = 0x00000002;
constexpr DWORD kInternetOptionConnectTimeout = 2;
constexpr DWORD kInternetOptionSendTimeout = 5;
constexpr DWORD kInternetOptionReceiveTimeout = 6;
constexpr DWORD kInternetErrorBase = 12000;
constexpr DWORD kErrorInternetTimeout = kInternetErrorBase + 2;
constexpr DWORD kErrorInternetExtended = kInternetErrorBase + 3;
constexpr DWORD kErrorInternetNameNotResolved = kInternetErrorBase + 7;
constexpr DWORD kErrorInternetIncorrectUserName = kInternetErrorBase + 13;
constexpr DWORD kErrorInternetIncorrectPassword = kInternetErrorBase + 14;
constexpr DWORD kErrorInternetLoginFailure = kInternetErrorBase + 15;
constexpr DWORD kErrorInternetCannotConnect = kInternetErrorBase + 29;
constexpr uint64_t kMaxMetadataDownloadBytes = 64ull * 1024ull * 1024ull;
constexpr uint64_t kMaxPayloadDownloadBytes = 16ull * 1024ull * 1024ull * 1024ull;
constexpr DWORD kMaxHttpResponseHeaderBytes = 64u * 1024u;
constexpr size_t kMaxHttpsRedirects = 5;
constexpr size_t kMaxCredentialCharacters = 1024;
constexpr size_t kMaxZipCentralEntries = 65534;
constexpr size_t kMaxZipExplicitDirectories = 65534;
constexpr uint64_t kMaxZipCentralDirectoryBytes = 256ull * 1024ull * 1024ull;

void check_cancelled(const CancellationCallback& cancel) {
    if (cancel) {
        cancel();
    }
}

bool is_cancelled_message(const std::string& message) {
    return message.find("Operation cancelled by user") != std::string::npos;
}

template <auto close_handle>
class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    InternetHandle(InternetHandle&& other) noexcept : value(other.release()) {}

    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ~InternetHandle() {
        reset();
    }

    void reset(HINTERNET handle = nullptr) noexcept {
        if (value) {
            close_handle(value);
        }
        value = handle;
    }

    HINTERNET release() noexcept {
        HINTERNET handle = value;
        value = nullptr;
        return handle;
    }

    explicit operator bool() const noexcept {
        return value != nullptr;
    }

    HINTERNET value = nullptr;
};

using WinHttpHandle = InternetHandle<WinHttpCloseHandle>;
using WinInetHandle = InternetHandle<InternetCloseHandle>;

std::string format_win32_error(DWORD error) {
    LPWSTR message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD length = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::string result = "Win32 error " + std::to_string(error);
    if (length && message) {
        int needed = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            std::vector<char> utf8(static_cast<size_t>(needed));
            int converted = WideCharToMultiByte(CP_UTF8,
                0,
                message,
                -1,
                utf8.data(),
                needed,
                nullptr,
                nullptr);
            if (converted > 0) {
                size_t utf8_length = static_cast<size_t>(converted);
                if (utf8_length > 0 && utf8[utf8_length - 1] == '\0') {
                    --utf8_length;
                }
                result += ": " + std::string(utf8.data(), utf8_length);
            }
        }
    }
    if (message) {
        LocalFree(message);
    }
    return result;
}

void require_win32(bool ok, const char* operation) {
    if (!ok) {
        throw AppError(std::string(operation) + " failed: " + format_win32_error(GetLastError()));
    }
}

std::string wide_to_utf8(const std::wstring& input);
std::wstring trim_w(std::wstring value);
uint64_t parse_uint64_decimal_strict(const std::wstring& value, const std::string& context);

const char* winhttp_error_name(DWORD error) {
    switch (error) {
    case ERROR_WINHTTP_TIMEOUT:
        return "ERROR_WINHTTP_TIMEOUT";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return "ERROR_WINHTTP_NAME_NOT_RESOLVED";
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return "ERROR_WINHTTP_CANNOT_CONNECT";
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return "ERROR_WINHTTP_CONNECTION_ERROR";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
        return "ERROR_WINHTTP_INVALID_SERVER_RESPONSE";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return "ERROR_WINHTTP_SECURE_FAILURE";
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return "ERROR_WINHTTP_OPERATION_CANCELLED";
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        return "ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED";
    default:
        return nullptr;
    }
}

const char* winhttp_error_hint(DWORD error) {
    switch (error) {
    case ERROR_WINHTTP_TIMEOUT:
        return "timeout; check server availability, proxy, firewall, and network latency";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return "DNS name was not resolved; check the source host name and DNS";
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return "cannot connect to server; check source URL, port, proxy, firewall, and whether this host accepts the selected HTTP/HTTPS transport";
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return "connection was closed or reset during transfer; check server stability, proxy, firewall, and retry";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
        return "server returned an invalid HTTP response; check that the endpoint is really HTTP/HTTPS";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return "TLS handshake or certificate validation failed; check HTTPS endpoint and Windows TLS support";
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return "operation was cancelled";
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        return "server requested a client certificate; client-certificate authentication is not implemented";
    default:
        return nullptr;
    }
}

std::string format_winhttp_error(DWORD error) {
    std::ostringstream message;
    if (const char* name = winhttp_error_name(error)) {
        message << name << " (" << error << ")";
        if (const char* hint = winhttp_error_hint(error)) {
            message << ": " << hint;
        }
    } else {
        message << format_win32_error(error);
    }
    return message.str();
}

const char* wininet_error_name(DWORD error) {
    switch (error) {
    case kErrorInternetTimeout:
        return "ERROR_INTERNET_TIMEOUT";
    case kErrorInternetExtended:
        return "ERROR_INTERNET_EXTENDED_ERROR";
    case kErrorInternetNameNotResolved:
        return "ERROR_INTERNET_NAME_NOT_RESOLVED";
    case kErrorInternetIncorrectUserName:
        return "ERROR_INTERNET_INCORRECT_USER_NAME";
    case kErrorInternetIncorrectPassword:
        return "ERROR_INTERNET_INCORRECT_PASSWORD";
    case kErrorInternetLoginFailure:
        return "ERROR_INTERNET_LOGIN_FAILURE";
    case kErrorInternetCannotConnect:
        return "ERROR_INTERNET_CANNOT_CONNECT";
    default:
        return nullptr;
    }
}

std::wstring last_wininet_response() {
    DWORD response_error = 0;
    DWORD length = 0;
    (void)InternetGetLastResponseInfoW(&response_error, nullptr, &length);
    if (length == 0) {
        return {};
    }

    std::wstring buffer(length, L'\0');
    if (!InternetGetLastResponseInfoW(&response_error, buffer.data(), &length)) {
        return {};
    }
    if (length < buffer.size()) {
        buffer.resize(length);
    }
    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return trim_w(std::move(buffer));
}

std::string format_wininet_error(DWORD error) {
    std::ostringstream message;
    if (const char* name = wininet_error_name(error)) {
        message << name << " (" << error << ")";
    } else {
        message << format_win32_error(error);
    }

    if (error == kErrorInternetIncorrectUserName ||
        error == kErrorInternetIncorrectPassword ||
        error == kErrorInternetLoginFailure) {
        message << ": FTP login rejected; check username/password in source settings";
    } else if (error == kErrorInternetNameNotResolved) {
        message << ": host name was not resolved; check source URL";
    } else if (error == kErrorInternetTimeout) {
        message << ": connection timed out";
    }

    std::wstring response = last_wininet_response();
    if (!response.empty()) {
        message << "; server response: " << wide_to_utf8(response);
    }
    return message.str();
}

void require_wininet(bool ok, const char* operation) {
    if (!ok) {
        DWORD error = GetLastError();
        throw AppError(std::string(operation) + " failed: " + format_wininet_error(error));
    }
}

void require_ntstatus(NTSTATUS status, const char* operation) {
    if (status < 0) {
        std::ostringstream message;
        message << operation << " failed: NTSTATUS 0x" << std::hex << static_cast<unsigned long>(status);
        throw AppError(message.str());
    }
}

std::wstring utf8_to_wide(const std::string& input) {
    if (input.empty()) {
        return {};
    }
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (needed <= 0) {
        throw AppError("Invalid UTF-8 text");
    }
    std::wstring output(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), output.data(), needed);
    return output;
}

std::string wide_to_utf8(const std::wstring& input) {
    if (input.empty()) {
        return {};
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw AppError("Could not convert UTF-16 to UTF-8");
    }
    std::string output(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), needed, nullptr, nullptr);
    return output;
}

std::string trim(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c) { return !is_space(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char c) { return !is_space(c); }).base(), value.end());
    return value;
}

std::wstring trim_w(std::wstring value) {
    auto is_space = [](wchar_t c) { return iswspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](wchar_t c) { return !is_space(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](wchar_t c) { return !is_space(c); }).base(), value.end());
    return value;
}

std::wstring trim_slashes(std::wstring value) {
    while (!value.empty() && (value.back() == L'/' || value.back() == L'\\')) {
        value.pop_back();
    }
    while (!value.empty() && (value.front() == L'/' || value.front() == L'\\')) {
        value.erase(value.begin());
    }
    return value;
}

bool starts_with_i(std::wstring_view value, std::wstring_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (towlower(value[i]) != towlower(prefix[i])) {
            return false;
        }
    }
    return true;
}

std::wstring lower_w(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

bool iequals_w(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (towlower(left[i]) != towlower(right[i])) {
            return false;
        }
    }
    return true;
}

bool url_has_authority_userinfo(const std::wstring& value) {
    std::wstring text = trim_w(value);
    size_t scheme = text.find(L"://");
    if (scheme == std::wstring::npos) {
        return false;
    }
    size_t authority_start = scheme + 3;
    size_t authority_end = text.find_first_of(L"/?#", authority_start);
    if (authority_end == std::wstring::npos) {
        authority_end = text.size();
    }
    size_t at = text.find(L'@', authority_start);
    return at != std::wstring::npos && at < authority_end;
}

void validate_url_has_no_userinfo(const std::wstring& value, const std::string& context) {
    if (url_has_authority_userinfo(value)) {
        throw AppError(context + " must not contain URL userinfo or embedded credentials");
    }
    for (wchar_t c : value) {
        if (c < 0x20 || c == 0x7f) {
            throw AppError(context + " contains a control character");
        }
    }
}

std::wstring redact_url_userinfo(const std::wstring& value) {
    std::wstring result = value;
    size_t scheme = result.find(L"://");
    if (scheme == std::wstring::npos) {
        return result;
    }
    size_t authority_start = scheme + 3;
    size_t authority_end = result.find_first_of(L"/?#", authority_start);
    if (authority_end == std::wstring::npos) {
        authority_end = result.size();
    }
    size_t at = result.find(L'@', authority_start);
    if (at != std::wstring::npos && at < authority_end) {
        result.replace(authority_start, at - authority_start, L"<redacted>");
    }
    return result;
}

std::wstring redact_server_userinfo(const std::wstring& value) {
    if (value.find(L"://") != std::wstring::npos) {
        return redact_url_userinfo(value);
    }
    std::wstring result = value;
    size_t at = result.find(L'@');
    size_t end = result.find_first_of(L"/\\?#");
    if (at != std::wstring::npos && (end == std::wstring::npos || at < end)) {
        result.replace(0, at, L"<redacted>");
    }
    return result;
}

void validate_server_has_no_userinfo(const std::wstring& server, const std::string& context) {
    std::wstring text = trim_w(server);
    size_t scheme = text.find(L"://");
    if (scheme != std::wstring::npos) {
        validate_url_has_no_userinfo(text, context);
        return;
    }
    size_t end = text.find_first_of(L"/\\?#");
    if (text.find(L'@') != std::wstring::npos && (end == std::wstring::npos || text.find(L'@') < end)) {
        throw AppError(context + " must not contain URL userinfo or embedded credentials");
    }
}

bool is_windows_reserved_device_component(const std::wstring& value) {
    std::wstring base = value.substr(0, value.find(L'.'));
    while (!base.empty() && (base.back() == L' ' || base.back() == L'.')) {
        base.pop_back();
    }
    base = lower_w(std::move(base));
    if (base == L"con" || base == L"prn" || base == L"aux" || base == L"nul" ||
        base == L"clock$" || base == L"conin$" || base == L"conout$") {
        return true;
    }
    if (base.size() == 4 && (base.compare(0, 3, L"com") == 0 || base.compare(0, 3, L"lpt") == 0)) {
        wchar_t suffix = base[3];
        return (suffix >= L'1' && suffix <= L'9') || suffix == L'\u00b9' || suffix == L'\u00b2' || suffix == L'\u00b3';
    }
    return false;
}

void validate_safe_path_component(const std::wstring& value, const std::string& context) {
    if (value.empty()) {
        throw AppError(context + " is empty");
    }
    if (trim_w(value) != value) {
        throw AppError(context + " has leading or trailing whitespace");
    }
    if (value == L"." || value == L"..") {
        throw AppError(context + " is not a safe path component");
    }
    if (value.back() == L'.' || value.back() == L' ') {
        throw AppError(context + " has a trailing dot or space");
    }
    for (wchar_t c : value) {
        if (c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*') {
            throw AppError(context + " contains a forbidden path character");
        }
    }
    fs::path path(value);
    if (path.has_root_name() || path.has_root_directory() || path.is_absolute() ||
        path.filename().wstring() != value || is_windows_reserved_device_component(value)) {
        throw AppError(context + " is not a safe Windows path component");
    }
}

bool is_safe_path_component(const std::wstring& value) {
    try {
        validate_safe_path_component(value, "path component");
        return true;
    } catch (const AppError&) {
        return false;
    }
}

std::wstring absolute_normalized_path(const fs::path& path) {
    std::wstring input = path.wstring();
    DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        throw AppError("GetFullPathName failed: " + format_win32_error(GetLastError()));
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1);
    DWORD written = GetFullPathNameW(input.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (written == 0 || written >= buffer.size()) {
        throw AppError("GetFullPathName failed: " + format_win32_error(GetLastError()));
    }
    std::wstring result(buffer.data(), written);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\') {
        result.pop_back();
    }
    return result;
}

void reject_existing_reparse_points(
    const std::wstring& normalized_root,
    const std::wstring& normalized_candidate,
    const std::string& context) {
    auto check_path = [&](const std::wstring& path) {
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                return;
            }
            throw AppError(context + " could not inspect path attributes for " +
                wide_to_utf8(path) + ": " + format_win32_error(error));
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw AppError(context + " crosses an existing reparse point");
        }
    };

    fs::path root_path(normalized_root);
    fs::path candidate_path(normalized_candidate);
    fs::path current = candidate_path.root_path();
    if (current.empty()) {
        throw AppError(context + " is not an absolute path");
    }
    std::wstring root_name = candidate_path.root_name().wstring();
    bool unc_server_root = root_name.size() > 2 &&
        root_name[0] == L'\\' && root_name[1] == L'\\' &&
        root_name.find(L'\\', 2) == std::wstring::npos &&
        root_name.find(L'/', 2) == std::wstring::npos;
    // For a UNC path, \\server\ is not a filesystem object by itself.
    // Start checking after the share component has been appended.
    if (!unc_server_root) {
        check_path(current.wstring());
    }
    for (const fs::path& component : candidate_path.relative_path()) {
        if (component.empty() || component == L".") {
            continue;
        }
        current /= component;
        check_path(current.wstring());
    }

    // Keep the root argument semantically checked even when candidate == root.
    check_path(root_path.wstring());
}

void reject_existing_reparse_points(const fs::path& path, const std::string& context) {
    std::wstring normalized = absolute_normalized_path(path);
    reject_existing_reparse_points(normalized, normalized, context);
}

fs::path ensure_path_within_root(const fs::path& root, const fs::path& candidate, const std::string& context) {
    std::wstring normalized_root = absolute_normalized_path(root);
    std::wstring normalized_candidate = absolute_normalized_path(candidate);
    std::wstring prefix = normalized_root;
    if (prefix.empty() || prefix.back() != L'\\') {
        prefix.push_back(L'\\');
    }
    if (normalized_candidate.size() <= prefix.size() || !starts_with_i(normalized_candidate, prefix)) {
        throw AppError(context + " escapes its configured root");
    }
    reject_existing_reparse_points(normalized_root, normalized_candidate, context);
    return fs::path(normalized_candidate);
}

fs::path safe_child_path(const fs::path& root, const std::wstring& component, const std::string& context) {
    validate_safe_path_component(component, context);
    return ensure_path_within_root(root, root / component, context);
}

std::wstring known_folder(REFKNOWNFOLDERID folder_id) {
    PWSTR raw = nullptr;
    HRESULT hr = SHGetKnownFolderPath(folder_id, 0, nullptr, &raw);
    if (FAILED(hr)) {
        throw AppError("SHGetKnownFolderPath failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }
    std::wstring result(raw);
    CoTaskMemFree(raw);
    return result;
}

fs::path app_data_dir() {
    return fs::path(known_folder(FOLDERID_RoamingAppData)) / kAppName;
}

fs::path local_app_data_dir() {
    return fs::path(known_folder(FOLDERID_LocalAppData)) / kAppName;
}

std::optional<fs::path> environment_path(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return std::nullopt;
    }
    std::wstring value(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) {
        return std::nullopt;
    }
    value.resize(written);
    value = trim_w(std::move(value));
    if (value.empty()) {
        return std::nullopt;
    }
    return fs::path(value);
}

fs::path environment_temp_dir() {
    if (auto path = environment_path(L"TMP")) {
        return *path / kAppName / L"temp";
    }
    if (auto path = environment_path(L"TEMP")) {
        return *path / kAppName / L"temp";
    }

    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0) {
            throw AppError("GetTempPath failed: " + format_win32_error(GetLastError()));
        }
        if (length < buffer.size()) {
            return fs::path(std::wstring(buffer.data(), length)) / kAppName / L"temp";
        }
        buffer.resize(length + 1);
    }
}

fs::path executable_dir() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw AppError("GetModuleFileName failed: " + format_win32_error(GetLastError()));
        }
        if (length < buffer.size()) {
            return fs::path(std::wstring(buffer.data(), length)).parent_path();
        }
        if (buffer.size() >= 32768) {
            throw AppError("Executable path is too long");
        }
        buffer.resize(buffer.size() * 2);
    }
}

fs::path config_path() {
    return executable_dir() / L"config.ini";
}

std::wstring build_bitness_name() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return L"arm64";
#elif defined(_WIN64)
    return L"x64";
#else
    return L"x86";
#endif
}

std::wstring app_user_agent() {
    return std::wstring(kAppName) + L"/" + utf8_to_wide(kAppVersion) + L"-" + build_bitness_name();
}

const char* app_version() noexcept {
    return kAppVersion;
}

const char* build_number() noexcept {
    return kBuildNumber;
}

std::wstring create_guid_string() {
    GUID guid{};
    HRESULT hr = CoCreateGuid(&guid);
    if (FAILED(hr)) {
        throw AppError("CoCreateGuid failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }

    wchar_t buffer[39]{};
    if (StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer))) == 0) {
        throw AppError("StringFromGUID2 failed");
    }

    std::wstring value(buffer);
    if (value.size() >= 2 && value.front() == L'{' && value.back() == L'}') {
        value = value.substr(1, value.size() - 2);
    }
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

std::string read_text_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw AppError("Could not open file for read: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_text_file(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw AppError("Could not open file for write: " + path.string());
    }
    output << data;
}

void write_text_file_atomic(const fs::path& path, const std::string& data) {
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }

    fs::path temp;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 100; ++attempt) {
        temp = path.parent_path() /
            (path.filename().wstring() +
                L".tmp." + std::to_wstring(GetCurrentProcessId()) +
                L"." + std::to_wstring(GetCurrentThreadId()) +
                L"." + std::to_wstring(GetTickCount64()) +
                L"." + std::to_wstring(attempt));
        file = CreateFileW(temp.wstring().c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            throw AppError("Could not create atomic temporary file: " + format_win32_error(GetLastError()));
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        throw AppError("Could not allocate a unique atomic temporary file");
    }

    auto discard_temp = [&]() noexcept {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
        if (!temp.empty()) {
            DeleteFileW(temp.wstring().c_str());
        }
    };

    try {
        size_t offset = 0;
        while (offset < data.size()) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(
                data.size() - offset,
                static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (!WriteFile(file, data.data() + offset, chunk, &written, nullptr) || written != chunk) {
                throw AppError("Could not write atomic temporary file: " + format_win32_error(GetLastError()));
            }
            offset += written;
        }
        if (!FlushFileBuffers(file)) {
            throw AppError("Could not flush atomic temporary file: " + format_win32_error(GetLastError()));
        }
        if (!CloseHandle(file)) {
            file = INVALID_HANDLE_VALUE;
            throw AppError("Could not close atomic temporary file: " + format_win32_error(GetLastError()));
        }
        file = INVALID_HANDLE_VALUE;

        if (!MoveFileExW(temp.wstring().c_str(),
                path.wstring().c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw AppError("Could not atomically replace file: " + format_win32_error(GetLastError()));
        }
        temp.clear();
    } catch (...) {
        discard_temp();
        throw;
    }
}

std::string base64_encode(const uint8_t* data, size_t size) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t block = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < size) {
            block |= static_cast<uint32_t>(data[i + 1]) << 8;
        }
        if (i + 2 < size) {
            block |= static_cast<uint32_t>(data[i + 2]);
        }
        output.push_back(alphabet[(block >> 18) & 0x3F]);
        output.push_back(alphabet[(block >> 12) & 0x3F]);
        output.push_back((i + 1 < size) ? alphabet[(block >> 6) & 0x3F] : '=');
        output.push_back((i + 2 < size) ? alphabet[block & 0x3F] : '=');
    }
    return output;
}

std::vector<uint8_t> base64_decode(const std::string& input) {
    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        if (c == '=') return -2;
        return -1;
    };

    std::vector<uint8_t> output;
    int value = 0;
    int bits = -8;
    for (char c : input) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        int decoded = decode_char(c);
        if (decoded == -2) {
            break;
        }
        if (decoded < 0) {
            throw AppError("Invalid base64 data");
        }
        value = (value << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<uint8_t>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return output;
}

template <typename Container>
class SecureContainerWiper {
public:
    explicit SecureContainerWiper(Container& value) noexcept : value_(&value) {}
    SecureContainerWiper(const SecureContainerWiper&) = delete;
    SecureContainerWiper& operator=(const SecureContainerWiper&) = delete;
    ~SecureContainerWiper() {
        if (value_ && !value_->empty()) {
            SecureZeroMemory(value_->data(), value_->size() * sizeof(typename Container::value_type));
        }
    }

private:
    Container* value_;
};

class LocalBlobGuard {
public:
    LocalBlobGuard(DATA_BLOB& blob, bool wipe) noexcept : blob_(&blob), wipe_(wipe) {}
    LocalBlobGuard(const LocalBlobGuard&) = delete;
    LocalBlobGuard& operator=(const LocalBlobGuard&) = delete;
    ~LocalBlobGuard() {
        if (blob_ && blob_->pbData) {
            if (wipe_ && blob_->cbData > 0) {
                SecureZeroMemory(blob_->pbData, blob_->cbData);
            }
            LocalFree(blob_->pbData);
            blob_->pbData = nullptr;
            blob_->cbData = 0;
        }
    }

private:
    DATA_BLOB* blob_;
    bool wipe_;
};

std::string protect_password(const std::wstring& password) {
    std::string utf8 = wide_to_utf8(password);
    SecureContainerWiper<std::string> utf8_wiper(utf8);
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(utf8.data());
    input.cbData = static_cast<DWORD>(utf8.size());

    DATA_BLOB output{};
    LocalBlobGuard output_guard(output, false);
    if (!CryptProtectData(&input, L"MMIDownloader password", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw AppError("CryptProtectData failed: " + format_win32_error(GetLastError()));
    }
    std::string encoded = base64_encode(output.pbData, output.cbData);
    return encoded;
}

std::wstring unprotect_password(const std::string& encoded) {
    if (trim(encoded).empty()) {
        return {};
    }
    std::vector<uint8_t> bytes = base64_decode(encoded);
    DATA_BLOB input{};
    input.pbData = bytes.data();
    input.cbData = static_cast<DWORD>(bytes.size());

    DATA_BLOB output{};
    LocalBlobGuard output_guard(output, true);
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw AppError("CryptUnprotectData failed: " + format_win32_error(GetLastError()));
    }
    std::string utf8(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureContainerWiper<std::string> utf8_wiper(utf8);
    return utf8_to_wide(utf8);
}

struct BCryptAlgHandle {
    BCRYPT_ALG_HANDLE value = nullptr;
    ~BCryptAlgHandle() {
        if (value) {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    }
};

struct BCryptKeyHandle {
    BCRYPT_KEY_HANDLE value = nullptr;
    ~BCryptKeyHandle() {
        if (value) {
            BCryptDestroyKey(value);
        }
    }
};

std::wstring decrypt_legacy_package_password(const std::wstring& encrypted) {
    if (trim_w(encrypted).empty()) {
        return {};
    }

    try {
        std::vector<uint8_t> cipher = base64_decode(wide_to_utf8(trim_w(encrypted)));
        static constexpr uint8_t salt[] = {162, 186, 18, 153, 55, 65, 10, 103, 195};
        const std::string base_password = "FRoaHAsSHAsVGhkLEhke";

        BCryptAlgHandle sha1;
        require_ntstatus(BCryptOpenAlgorithmProvider(&sha1.value, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
            "BCryptOpenAlgorithmProvider(SHA1)");

        std::vector<uint8_t> derived(48);
        SecureContainerWiper<std::vector<uint8_t>> derived_wiper(derived);
        require_ntstatus(BCryptDeriveKeyPBKDF2(sha1.value,
            reinterpret_cast<PUCHAR>(const_cast<char*>(base_password.data())),
            static_cast<ULONG>(base_password.size()),
            const_cast<PUCHAR>(salt),
            static_cast<ULONG>(sizeof(salt)),
            1000,
            derived.data(),
            static_cast<ULONG>(derived.size()),
            0),
            "BCryptDeriveKeyPBKDF2");

        BCryptAlgHandle aes;
        require_ntstatus(BCryptOpenAlgorithmProvider(&aes.value, BCRYPT_AES_ALGORITHM, nullptr, 0),
            "BCryptOpenAlgorithmProvider(AES)");
        require_ntstatus(BCryptSetProperty(aes.value,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
            static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_ECB) + 1) * sizeof(wchar_t)),
            0),
            "BCryptSetProperty(AES ECB)");

        BCryptKeyHandle key;
        require_ntstatus(BCryptGenerateSymmetricKey(aes.value,
            &key.value,
            nullptr,
            0,
            derived.data(),
            32,
            0),
            "BCryptGenerateSymmetricKey");

        ULONG plain_size = 0;
        require_ntstatus(BCryptDecrypt(key.value,
            cipher.data(),
            static_cast<ULONG>(cipher.size()),
            nullptr,
            nullptr,
            0,
            nullptr,
            0,
            &plain_size,
            BCRYPT_BLOCK_PADDING),
            "BCryptDecrypt(size)");

        std::vector<uint8_t> plain(plain_size);
        SecureContainerWiper<std::vector<uint8_t>> plain_wiper(plain);
        require_ntstatus(BCryptDecrypt(key.value,
            cipher.data(),
            static_cast<ULONG>(cipher.size()),
            nullptr,
            nullptr,
            0,
            plain.data(),
            static_cast<ULONG>(plain.size()),
            &plain_size,
            BCRYPT_BLOCK_PADDING),
            "BCryptDecrypt(data)");
        plain.resize(plain_size);
        std::string plain_utf8(reinterpret_cast<const char*>(plain.data()), plain.size());
        SecureContainerWiper<std::string> plain_utf8_wiper(plain_utf8);
        return utf8_to_wide(plain_utf8);
    } catch (const std::exception&) {
        return encrypted;
    }
}

Config::Config()
    : metadata_dir(executable_dir() / L"metadata"),
      temp_dir(environment_temp_dir()),
      log_dir(executable_dir() / L"Logs") {}

bool config_has_credentials(const Config& cfg) {
    return !trim_w(cfg.username).empty() || !cfg.password_dpapi.empty();
}

void validate_config_credentials_complete(const Config& cfg) {
    bool has_username = !trim_w(cfg.username).empty();
    bool has_password = !cfg.password_dpapi.empty();
    if (has_username != has_password) {
        throw AppError("Configured credentials require both username and DPAPI password");
    }
}

void validate_config_network_fields(const Config& cfg) {
    validate_config_credentials_complete(cfg);
    validate_server_has_no_userinfo(cfg.server, "Config server");
    if (!trim_w(cfg.catalog_root).empty()) {
        validate_url_has_no_userinfo(cfg.catalog_root, "Config network URL");
    }
}

void validate_credentials_for_transport(const Config& cfg, bool encrypted, const char* transport) {
    validate_config_credentials_complete(cfg);
    if (config_has_credentials(cfg) && !encrypted) {
        throw AppError(std::string("Configured credentials are forbidden for plaintext ") + transport + "; use HTTPS");
    }
}

class SecureWideString {
public:
    explicit SecureWideString(std::wstring value) : value_(std::move(value)) {}
    SecureWideString(const SecureWideString&) = delete;
    SecureWideString& operator=(const SecureWideString&) = delete;
    ~SecureWideString() {
        clear();
    }

    const std::wstring& value() const {
        return value_;
    }

    void clear() noexcept {
        if (!value_.empty()) {
            SecureZeroMemory(value_.data(), value_.size() * sizeof(wchar_t));
            value_.clear();
        }
    }

private:
    std::wstring value_;
};

std::wstring normalize_server(std::wstring server) {
    server = trim_w(server);
    if (starts_with_i(server, L"https://")) {
        server.erase(0, 8);
    } else if (starts_with_i(server, L"http://")) {
        server.erase(0, 7);
    }
    size_t slash = server.find_first_of(L"/\\");
    if (slash != std::wstring::npos) {
        server.erase(slash);
    }
    return trim_slashes(server);
}

std::wstring bool_to_transport(bool https) {
    return https ? L"https" : L"http";
}

bool transport_to_bool(const std::wstring& value) {
    std::wstring v = trim_w(value);
    std::transform(v.begin(), v.end(), v.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (v == L"https" || v == L"1" || v == L"true") {
        return true;
    }
    if (v == L"http" || v == L"0" || v == L"false") {
        return false;
    }
    throw AppError("Invalid transport value. Use https or http.");
}

bool looks_like_url(const std::wstring& value) {
    std::wstring v = trim_w(value);
    return starts_with_i(v, L"http://") || starts_with_i(v, L"https://") || starts_with_i(v, L"ftp://");
}

bool looks_like_http_url(const std::wstring& value) {
    std::wstring v = trim_w(value);
    return starts_with_i(v, L"http://") || starts_with_i(v, L"https://");
}

bool looks_like_ftp_url(const std::wstring& value) {
    return starts_with_i(trim_w(value), L"ftp://");
}

bool config_bool_from_string(const std::wstring& value) {
    std::wstring v = trim_w(value);
    std::transform(v.begin(), v.end(), v.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (v == L"1" || v == L"true" || v == L"yes" || v == L"on") {
        return true;
    }
    if (v == L"0" || v == L"false" || v == L"no" || v == L"off") {
        return false;
    }
    throw AppError("Invalid boolean value in config: " + wide_to_utf8(value));
}

std::wstring config_bool_to_string(bool value) {
    return value ? L"true" : L"false";
}

std::wstring source_transport_to_string(SourceTransport value) {
    switch (value) {
    case SourceTransport::Auto:
        return L"auto";
    case SourceTransport::WebDav:
        return L"webdav";
    case SourceTransport::File:
        return L"file";
    case SourceTransport::Url:
        return L"url";
    case SourceTransport::Ftp:
        return L"ftp";
    default:
        return L"auto";
    }
}

SourceTransport source_transport_from_string(const std::wstring& value) {
    std::wstring v = trim_w(value);
    std::transform(v.begin(), v.end(), v.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (v.empty() || v == L"auto" || v == L"detect") {
        return SourceTransport::Auto;
    }
    if (v == L"webdav" || v == L"dav") {
        return SourceTransport::WebDav;
    }
    if (v == L"file" || v == L"local" || v == L"path") {
        return SourceTransport::File;
    }
    if (v == L"url" || v == L"http" || v == L"https") {
        return SourceTransport::Url;
    }
    if (v == L"ftp") {
        return SourceTransport::Ftp;
    }
    throw AppError("Invalid source_transport value. Use auto, webdav, file, url, or ftp.");
}

std::wstring ftp_mode_to_string(FtpMode value) {
    switch (value) {
    case FtpMode::Auto:
        return L"auto";
    case FtpMode::Passive:
        return L"passive";
    case FtpMode::Active:
        return L"active";
    default:
        return L"auto";
    }
}

FtpMode ftp_mode_from_string(const std::wstring& value) {
    std::wstring v = trim_w(value);
    std::transform(v.begin(), v.end(), v.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (v.empty() || v == L"auto") {
        return FtpMode::Auto;
    }
    if (v == L"passive" || v == L"pasv") {
        return FtpMode::Passive;
    }
    if (v == L"active" || v == L"port") {
        return FtpMode::Active;
    }
    throw AppError("Invalid ftp_mode value. Use auto, passive, or active.");
}

std::wstring trim_trailing_url_slashes(std::wstring value) {
    value = trim_w(std::move(value));
    while (!value.empty() && (value.back() == L'/' || value.back() == L'\\')) {
        value.pop_back();
    }
    return value;
}

std::optional<Config> load_config_if_exists() {
    Config cfg;
    fs::path path = config_path();
    if (!fs::exists(path)) {
        return std::nullopt;
    }

    std::istringstream input(read_text_file(path));
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::string key = trim(line.substr(0, equals));
        std::string value = trim(line.substr(equals + 1));
        std::wstring wide_value = utf8_to_wide(value);

        if (key == "client_id") cfg.client_id = trim_w(wide_value);
        else if (key == "protocol") cfg.https = transport_to_bool(wide_value);
        else if (key == "server") cfg.server = normalize_server(wide_value);
        else if (key == "webdav_collection") cfg.top_level_collection = trim_slashes(wide_value);
        else if (key == "webdav_channel") cfg.distribution_channel = trim_slashes(wide_value);
        else if (key == "source") {
            cfg.source_transport = source_transport_from_string(wide_value);
        }
        else if (key == "network") cfg.catalog_root = trim_w(wide_value);
        else if (key == "use_local") cfg.use_local_catalog = config_bool_from_string(wide_value);
        else if (key == "local") cfg.local_catalog_root = fs::path(wide_value);
        else if (key == "ftp_mode") cfg.ftp_mode = ftp_mode_from_string(wide_value);
        else if (key == "username") cfg.username = wide_value;
        else if (key == "password") cfg.password_dpapi = value;
        else if (key == "metadata") {
            cfg.metadata_dir = fs::path(wide_value);
        }
        else if (key == "temp") {
            cfg.temp_dir = fs::path(wide_value);
        }
        else if (key == "extract") {
            cfg.extract_dir = fs::path(wide_value);
        }
        else if (key == "logging") cfg.logging_enabled = config_bool_from_string(wide_value);
        else if (key == "log") cfg.log_dir = fs::path(wide_value);
    }

    if (cfg.source_transport == SourceTransport::File) {
        cfg.use_local_catalog = true;
        cfg.source_transport = SourceTransport::Auto;
    }
    if (cfg.use_local_catalog && cfg.local_catalog_root.empty()) {
        throw AppError("Config key 'local' is empty while use_local=true");
    }
    if (cfg.log_dir.empty()) {
        cfg.log_dir = executable_dir() / L"Logs";
    }
    validate_config_network_fields(cfg);
    return cfg;
}

void save_config(const Config& cfg) {
    validate_config_network_fields(cfg);
    std::ostringstream output;
    output << "# MMIDownloader native configuration\n";
    output << "# password is encrypted by Windows DPAPI for the current user.\n";
    output << "client_id=" << wide_to_utf8(trim_w(cfg.client_id)) << "\n";
    output << "source=" << wide_to_utf8(source_transport_to_string(cfg.source_transport)) << "\n";
    output << "network=" << wide_to_utf8(trim_w(cfg.catalog_root)) << "\n";
    output << "use_local=" << wide_to_utf8(config_bool_to_string(cfg.use_local_catalog)) << "\n";
    output << "local=" << wide_to_utf8(cfg.local_catalog_root.wstring()) << "\n";
    output << "ftp_mode=" << wide_to_utf8(ftp_mode_to_string(cfg.ftp_mode)) << "\n";
    output << "protocol=" << wide_to_utf8(bool_to_transport(cfg.https)) << "\n";
    output << "server=" << wide_to_utf8(normalize_server(cfg.server)) << "\n";
    output << "username=" << wide_to_utf8(cfg.username) << "\n";
    output << "password=" << cfg.password_dpapi << "\n";
    output << "webdav_collection=" << wide_to_utf8(trim_slashes(cfg.top_level_collection)) << "\n";
    output << "webdav_channel=" << wide_to_utf8(trim_slashes(cfg.distribution_channel)) << "\n";
    output << "metadata=" << wide_to_utf8(cfg.metadata_dir.wstring()) << "\n";
    output << "temp=" << wide_to_utf8(cfg.temp_dir.wstring()) << "\n";
    output << "extract=" << wide_to_utf8(cfg.extract_dir.wstring()) << "\n";
    output << "logging=" << wide_to_utf8(config_bool_to_string(cfg.logging_enabled)) << "\n";
    output << "log=" << wide_to_utf8(cfg.log_dir.wstring()) << "\n";
    write_text_file_atomic(config_path(), output.str());
}

Config load_config() {
    std::optional<Config> loaded = load_config_if_exists();
    bool existed_when_read = loaded.has_value();
    Config cfg = existed_when_read ? std::move(*loaded) : Config{};
    bool changed = false;
    if (trim_w(cfg.client_id).empty()) {
        cfg.client_id = create_guid_string();
        changed = true;
    }
    if (existed_when_read && changed) {
        save_config(cfg);
    }
    return cfg;
}

const wchar_t* operation_status_name(OperationStatus status) {
    switch (status) {
    case OperationStatus::Success: return L"Success";
    case OperationStatus::NoRecommendedCapacity: return L"NoRecommendedCapacity";
    case OperationStatus::SpeedNotSuffient: return L"SpeedNotSuffient";
    case OperationStatus::SdCardFormatFailed: return L"SdCardFormatFailed";
    case OperationStatus::BtacNoWebDavConnection: return L"BtacNoWebDavConnection";
    case OperationStatus::LocalStorageNotEnough: return L"LocalStorageNotEnough";
    case OperationStatus::PackageExtractFailed: return L"PackageExtractFailed";
    case OperationStatus::PackageHashValidationFailed: return L"PackageHashValidationFailed";
    case OperationStatus::PackageVerifyFailed: return L"PackageVerifyFailed";
    case OperationStatus::VerifyStructureNotFound: return L"VerifyStructureNotFound";
    case OperationStatus::CardEjectFailed: return L"CardEjectFailed";
    case OperationStatus::CancelByUser: return L"CancelByUser";
    case OperationStatus::Exception: return L"Exception";
    case OperationStatus::MetaFileError: return L"MetaFileError";
    default: return L"Exception";
    }
}

std::string format_log_time(std::chrono::system_clock::time_point value) {
    std::time_t raw = std::chrono::system_clock::to_time_t(value);
    std::tm local{};
    localtime_s(&local, &raw);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%d.%m.%Y, %H:%M:%S", &local);
    return buffer;
}

std::string today_log_suffix() {
    std::time_t raw = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &raw);
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d", &local);
    return buffer;
}

std::string text_log_escape(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\r': result += "\\r"; break;
        case '\n': result += "\\n"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                result += ' ';
            } else {
                result.push_back(c);
            }
            break;
        }
    }
    return result;
}

fs::path log_dir(const Config& cfg) {
    return cfg.log_dir.empty() ? executable_dir() / L"Logs" : cfg.log_dir;
}

void append_log_field(std::ostringstream& output, const char* key, const std::string& value) {
    output << ' ' << key << "=\"" << text_log_escape(value) << "\"";
}

void append_log_field(std::ostringstream& output, const char* key, const std::wstring& value) {
    append_log_field(output, key, wide_to_utf8(value));
}

void append_text_log_entry(const fs::path& path, const std::string& entry) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        throw AppError("Could not open log file for append: " + wide_to_utf8(path.wstring()));
    }
    output << entry;
    if (!entry.empty() && entry.back() != '\n') {
        output << '\n';
    }
    if (!output) {
        throw AppError("Could not write log file: " + wide_to_utf8(path.wstring()));
    }
}

template <typename Callback>
bool best_effort_log(Callback&& callback) noexcept {
    try {
        callback();
        return true;
    } catch (...) {
        return false;
    }
}

bool log_error_entry(const Config& cfg,
    OperationStatus status,
    const std::wstring& message,
    const std::wstring& level) noexcept {
    if (!cfg.logging_enabled) {
        return true;
    }
    return best_effort_log([&]() {
        fs::path path = log_dir(cfg) / fs::path(utf8_to_wide("error_" + today_log_suffix() + ".log"));
        std::ostringstream entry;
        entry << format_log_time(std::chrono::system_clock::now()) << " error";
        append_log_field(entry, "client_id", cfg.client_id);
        append_log_field(entry, "version", kAppVersion);
        append_log_field(entry, "status", operation_status_name(status));
        append_log_field(entry, "level", level);
        append_log_field(entry, "message", message);
        entry << "\n";
        append_text_log_entry(path, entry.str());
    });
}

bool log_operation_report(const Config& cfg,
    const std::wstring& brand,
    const std::wstring& part_number,
    const std::wstring& file_name,
    std::chrono::system_clock::time_point start,
    std::chrono::system_clock::time_point end,
    OperationStatus status) noexcept {
    if (!cfg.logging_enabled) {
        return true;
    }
    return best_effort_log([&]() {
        fs::path path = log_dir(cfg) / fs::path(utf8_to_wide("report_" + today_log_suffix() + ".log"));
        std::ostringstream entry;
        entry << format_log_time(std::chrono::system_clock::now()) << " operation";
        append_log_field(entry, "client_id", cfg.client_id);
        append_log_field(entry, "version", kAppVersion);
        append_log_field(entry, "brand", brand);
        append_log_field(entry, "part", part_number);
        append_log_field(entry, "file", file_name);
        append_log_field(entry, "start", format_log_time(start));
        append_log_field(entry, "end", format_log_time(end));
        append_log_field(entry, "status", operation_status_name(status));
        entry << "\n";
        append_text_log_entry(path, entry.str());
    });
}

OperationStatus classify_operation_failure(OperationStatus fallback, const std::string& message) {
    if (is_cancelled_message(message)) {
        return OperationStatus::CancelByUser;
    }
    if (message.find("MD5 mismatch") != std::string::npos) {
        return OperationStatus::PackageHashValidationFailed;
    }
    if (message.find("Package count mismatch") != std::string::npos ||
        message.find("No ZIP payload") != std::string::npos) {
        return OperationStatus::PackageVerifyFailed;
    }
    if (message.find("VerifyData") != std::string::npos) {
        return OperationStatus::PackageVerifyFailed;
    }
    if (message.find("Internet") != std::string::npos ||
        message.find("WinHttp") != std::string::npos ||
        message.find("HTTP ") != std::string::npos ||
        message.find("FTP") != std::string::npos) {
        return OperationStatus::BtacNoWebDavConnection;
    }
    return fallback;
}

std::wstring webdav_base_path(const Config& cfg) {
    return L"/dav/" + trim_slashes(cfg.top_level_collection) + L"/" + trim_slashes(cfg.distribution_channel) + L"/";
}

std::wstring webdav_home_url(const Config& cfg) {
    validate_server_has_no_userinfo(cfg.server, "WebDAV server");
    return bool_to_transport(cfg.https) + L"://" + normalize_server(cfg.server) + webdav_base_path(cfg);
}

std::wstring combine_webdav_path(const Config& cfg, std::wstring relative) {
    relative = trim_slashes(std::move(relative));
    return webdav_base_path(cfg) + relative;
}

std::wstring webdav_full_url(const Config& cfg, const std::wstring& relative) {
    validate_server_has_no_userinfo(cfg.server, "WebDAV server");
    return bool_to_transport(cfg.https) + L"://" + normalize_server(cfg.server) + combine_webdav_path(cfg, relative);
}

std::wstring catalog_channel_relative_path(const Config& cfg, const std::wstring& relative) {
    std::wstring channel = trim_slashes(cfg.distribution_channel);
    std::wstring path = trim_slashes(relative);
    if (channel.empty()) {
        return path;
    }
    if (path.empty()) {
        return channel;
    }
    return channel + L"/" + path;
}

std::wstring catalog_network_url_full(const Config& cfg, const std::wstring& relative) {
    std::wstring root = trim_trailing_url_slashes(cfg.catalog_root);
    if (root.empty() || !looks_like_url(root)) {
        throw AppError("Network catalog root requires http://, https://, or ftp://");
    }
    validate_url_has_no_userinfo(root, "Network catalog root");
    return root + L"/" + catalog_channel_relative_path(cfg, relative);
}

bool legacy_webdav_source_is_configured(const Config& cfg) {
    return !normalize_server(cfg.server).empty() &&
        !trim_slashes(cfg.top_level_collection).empty() &&
        !trim_slashes(cfg.distribution_channel).empty();
}

SourceTransport effective_source_transport(const Config& cfg) {
    if (cfg.use_local_catalog || cfg.source_transport == SourceTransport::File) {
        if (cfg.local_catalog_root.empty()) {
            throw AppError("Local catalog root is not configured");
        }
        return SourceTransport::File;
    }

    std::wstring network = trim_w(cfg.catalog_root);
    switch (cfg.source_transport) {
    case SourceTransport::WebDav:
        if (!legacy_webdav_source_is_configured(cfg)) {
            throw AppError("WebDAV source requires server, webdav_collection, and webdav_channel");
        }
        return SourceTransport::WebDav;
    case SourceTransport::Url:
        if (!looks_like_http_url(network)) {
            throw AppError("HTTP catalog source requires an http:// or https:// URL");
        }
        return SourceTransport::Url;
    case SourceTransport::Ftp:
        if (!looks_like_ftp_url(network)) {
            throw AppError("FTP catalog source requires an ftp:// URL");
        }
        return SourceTransport::Ftp;
    case SourceTransport::File:
        break;
    case SourceTransport::Auto:
        break;
    }

    if (looks_like_ftp_url(network)) {
        return SourceTransport::Ftp;
    }
    if (looks_like_http_url(network)) {
        return SourceTransport::Url;
    }
    if (!network.empty()) {
        throw AppError("Network catalog source requires an http://, https://, or ftp:// URL");
    }
    if (legacy_webdav_source_is_configured(cfg)) {
        return SourceTransport::WebDav;
    }
    throw AppError("Catalog source is not configured");
}

bool catalog_source_is_configured(const Config& cfg) {
    if (cfg.use_local_catalog || cfg.source_transport == SourceTransport::File) {
        return !cfg.local_catalog_root.empty();
    }
    switch (cfg.source_transport) {
    case SourceTransport::WebDav:
        return legacy_webdav_source_is_configured(cfg);
    case SourceTransport::Url:
    case SourceTransport::Ftp:
        return !trim_w(cfg.catalog_root).empty();
    case SourceTransport::File:
        return !cfg.local_catalog_root.empty();
    case SourceTransport::Auto:
        return !trim_w(cfg.catalog_root).empty() || legacy_webdav_source_is_configured(cfg);
    default:
        return false;
    }
}

bool credentials_target_an_https_source(const Config& cfg) {
    SourceTransport transport = effective_source_transport(cfg);
    return (transport == SourceTransport::WebDav && cfg.https) ||
        (transport == SourceTransport::Url && starts_with_i(trim_w(cfg.catalog_root), L"https://"));
}

std::vector<std::wstring> split_relative_path(std::wstring relative) {
    relative = trim_slashes(std::move(relative));
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start < relative.size()) {
        size_t pos = relative.find_first_of(L"/\\", start);
        std::wstring part = relative.substr(start, pos == std::wstring::npos ? std::wstring::npos : pos - start);
        if (!part.empty() && part != L"." && part != L"..") {
            parts.push_back(std::move(part));
        }
        if (pos == std::wstring::npos) {
            break;
        }
        start = pos + 1;
    }
    return parts;
}

fs::path catalog_file_source_path(const Config& cfg, const std::wstring& webdav_relative) {
    if (!cfg.local_catalog_root.empty()) {
        fs::path path = cfg.local_catalog_root;
        for (const std::wstring& part : split_relative_path(catalog_channel_relative_path(cfg, webdav_relative))) {
            path /= part;
        }
        return path;
    }
    throw AppError("Local catalog root is empty");
}

fs::path mirror_source_path(const Config& cfg, const std::wstring& webdav_relative) {
    return catalog_file_source_path(cfg, webdav_relative);
}

struct CatalogLocation {
    SourceTransport transport = SourceTransport::WebDav;
    fs::path file_path;
    std::wstring url;
};

CatalogLocation resolve_catalog_location(const Config& cfg, const std::wstring& webdav_relative) {
    CatalogLocation location;
    location.transport = effective_source_transport(cfg);
    switch (location.transport) {
    case SourceTransport::WebDav:
        location.url = webdav_full_url(cfg, webdav_relative);
        break;
    case SourceTransport::File:
        location.file_path = catalog_file_source_path(cfg, webdav_relative);
        break;
    case SourceTransport::Url:
        location.url = catalog_network_url_full(cfg, webdav_relative);
        break;
    case SourceTransport::Ftp:
        location.url = catalog_network_url_full(cfg, webdav_relative);
        break;
    default:
        throw AppError("Unsupported source_transport value");
    }
    return location;
}

std::wstring catalog_home_description(const Config& cfg) {
    switch (effective_source_transport(cfg)) {
    case SourceTransport::WebDav:
        return webdav_home_url(cfg);
    case SourceTransport::File:
        if (!cfg.local_catalog_root.empty()) {
            fs::path path = cfg.local_catalog_root;
            for (const std::wstring& part : split_relative_path(catalog_channel_relative_path(cfg, L""))) {
                path /= part;
            }
            return path.wstring();
        }
        return L"";
    case SourceTransport::Url:
    case SourceTransport::Ftp:
        return catalog_network_url_full(cfg, L"");
    default:
        return L"";
    }
}

std::wstring catalog_location_description(const Config& cfg, const std::wstring& webdav_relative) {
    CatalogLocation location = resolve_catalog_location(cfg, webdav_relative);
    if (location.transport == SourceTransport::File) {
        return location.file_path.wstring();
    }
    return location.url;
}

std::string printable_path(const fs::path& path) {
    return wide_to_utf8(path.wstring());
}

bool file_exists_nonempty(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return false;
    }
    uintmax_t size = fs::file_size(path, ec);
    return !ec && size > 0;
}

void remove_empty_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return;
    }
    uintmax_t size = fs::file_size(path, ec);
    if (!ec && size == 0) {
        fs::remove(path, ec);
    }
}

fs::path download_temp_path(const fs::path& target) {
    std::wstring name = target.filename().wstring() +
        L".download." +
        std::to_wstring(GetCurrentProcessId()) +
        L"." +
        std::to_wstring(GetCurrentThreadId());
    return target.parent_path() / name;
}

fs::path download_partial_path(const fs::path& target) {
    return safe_child_path(target.parent_path(),
        target.filename().wstring() + L".partial",
        "Download partial file");
}

uint64_t file_size_or_zero(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return 0;
    }
    uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        return 0;
    }
    return static_cast<uint64_t>(size);
}

void replace_with_downloaded_file(const fs::path& temp, const fs::path& target) {
    std::wstring temp_text = temp.wstring();
    std::wstring target_text = target.wstring();
    require_win32(MoveFileExW(temp_text.c_str(),
        target_text.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
        "MoveFileEx(download)");
}

std::mutex& network_transfer_mutex() {
    static std::mutex mutex;
    return mutex;
}

struct ParsedHttpUrl {
    bool https = true;
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
};

ParsedHttpUrl parse_http_url_strict(const std::wstring& url) {
    validate_url_has_no_userinfo(url, "HTTP URL");
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    require_win32(WinHttpCrackUrl(url.c_str(), 0, 0, &components), "WinHttpCrackUrl");

    if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) {
        throw AppError("URL scheme must be HTTP or HTTPS");
    }
    if ((components.lpszUserName && components.dwUserNameLength > 0) ||
        (components.lpszPassword && components.dwPasswordLength > 0)) {
        throw AppError("HTTP URL must not contain embedded credentials");
    }
    if (!components.lpszHostName || components.dwHostNameLength == 0 || components.nPort == 0) {
        throw AppError("HTTP URL host or port is empty");
    }

    ParsedHttpUrl parsed;
    parsed.https = components.nScheme == INTERNET_SCHEME_HTTPS;
    parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed.port = components.nPort;
    if (components.lpszUrlPath && components.dwUrlPathLength > 0) {
        parsed.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo && components.dwExtraInfoLength > 0) {
        parsed.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    size_t fragment = parsed.path.find(L'#');
    if (fragment != std::wstring::npos) {
        parsed.path.erase(fragment);
    }
    if (parsed.path.empty()) {
        parsed.path = L"/";
    }
    return parsed;
}

bool same_https_origin(const ParsedHttpUrl& left, const ParsedHttpUrl& right) {
    return left.https && right.https && left.port == right.port && iequals_w(left.host, right.host);
}

std::wstring combine_redirect_url(const std::wstring& base_url, const std::wstring& location) {
    std::wstring trimmed_location = trim_w(location);
    if (trimmed_location.empty()) {
        throw AppError("HTTP redirect has an empty Location header");
    }
    validate_url_has_no_userinfo(trimmed_location, "HTTP redirect Location");
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    HRESULT hr = UrlCombineW(base_url.c_str(), trimmed_location.c_str(), buffer.data(), &length, 0);
    if (FAILED(hr)) {
        throw AppError("Could not resolve HTTP redirect Location: HRESULT " +
            std::to_string(static_cast<unsigned long>(hr)));
    }
    std::wstring combined(buffer.data(), length);
    (void)parse_http_url_strict(combined);
    return combined;
}

class WinHttpClient {
public:
    explicit WinHttpClient(Config config, CancellationCallback cancel = {})
        : cfg_(std::move(config)), cancel_(std::move(cancel)) {}

    DWORD get_to_file(const std::wstring& webdav_relative,
        const fs::path& target,
        uint64_t max_bytes = kMaxPayloadDownloadBytes) {
        if (max_bytes == 0) {
            throw AppError("Download byte limit must be positive");
        }
        mmid::check_cancelled(cancel_);
        CatalogLocation location = resolve_catalog_location(cfg_, webdav_relative);
        reject_existing_reparse_points(target, "Download target");
        fs::create_directories(target.parent_path());
        reject_existing_reparse_points(target.parent_path(), "Download target directory");
        fs::path temp = download_temp_path(target);
        std::error_code cleanup_error;
        fs::remove(temp, cleanup_error);

        if (location.transport == SourceTransport::File) {
            fs::path source = location.file_path;
            reject_existing_reparse_points(source, "Catalog source");
            if (!fs::exists(source)) {
                throw AppError("Catalog source not found: " + printable_path(source));
            }
            if (!fs::is_regular_file(source)) {
                throw AppError("Catalog source is not a regular file: " + printable_path(source));
            }
            std::error_code size_error;
            uintmax_t source_size = fs::file_size(source, size_error);
            if (size_error) {
                throw AppError("Could not read catalog source size: " + size_error.message());
            }
            if (source_size > max_bytes) {
                throw AppError("Catalog source exceeds configured download byte limit");
            }
            try {
                mmid::check_cancelled(cancel_);
                fs::copy_file(source, temp, fs::copy_options::overwrite_existing);
                mmid::check_cancelled(cancel_);
                replace_with_downloaded_file(temp, target);
            } catch (...) {
                fs::remove(temp, cleanup_error);
                throw;
            }
            return 200;
        }

        DWORD status = 0;
        try {
            {
                mmid::check_cancelled(cancel_);
                std::ofstream output(temp, std::ios::binary | std::ios::trunc);
                if (!output) {
                    throw AppError("Could not open target file: " + printable_path(temp));
                }
                if (location.transport == SourceTransport::Ftp) {
                    status = request_ftp_url(location.url, &output, max_bytes);
                } else {
                    status = request_url(L"GET", location.url, &output, max_bytes);
                }
                output.close();
                if (!output) {
                    throw AppError("Could not write target file: " + printable_path(temp));
                }
            }
            mmid::check_cancelled(cancel_);
            replace_with_downloaded_file(temp, target);
        } catch (...) {
            fs::remove(temp, cleanup_error);
            throw;
        }
        return status;
    }

    DWORD get_to_file_resumable(const std::wstring& webdav_relative,
        const fs::path& partial_target,
        uint64_t max_bytes = kMaxPayloadDownloadBytes) {
        if (max_bytes == 0) {
            throw AppError("Download byte limit must be positive");
        }
        mmid::check_cancelled(cancel_);
        CatalogLocation location = resolve_catalog_location(cfg_, webdav_relative);
        reject_existing_reparse_points(partial_target, "Download target");
        fs::create_directories(partial_target.parent_path());
        reject_existing_reparse_points(partial_target.parent_path(), "Download target directory");

        if (location.transport == SourceTransport::File || location.transport == SourceTransport::Ftp) {
            return get_to_file(webdav_relative, partial_target, max_bytes);
        }

        DWORD status = 0;
        for (size_t restart = 0; restart < 2; ++restart) {
            mmid::check_cancelled(cancel_);
            uint64_t resume_offset = file_size_or_zero(partial_target);
            if (resume_offset > max_bytes) {
                std::error_code cleanup_error;
                fs::remove(partial_target, cleanup_error);
                throw AppError("Partial download exceeds configured download byte limit");
            }

            std::ofstream output(partial_target,
                std::ios::binary | (resume_offset > 0 ? std::ios::app : std::ios::trunc));
            if (!output) {
                throw AppError("Could not open target file: " + printable_path(partial_target));
            }

            try {
                status = request_url(L"GET", location.url, &output, max_bytes, resume_offset);
                output.close();
                if (!output) {
                    throw AppError("Could not write target file: " + printable_path(partial_target));
                }
                return status;
            } catch (const std::exception& ex) {
                output.close();
                std::string message = ex.what();
                if (resume_offset > 0 &&
                    (message.find("HTTP Range resume failed: server ignored Range request") != std::string::npos ||
                     message.find("HTTP Range resume failed: server rejected resume offset") != std::string::npos)) {
                    std::error_code cleanup_error;
                    fs::remove(partial_target, cleanup_error);
                    continue;
                }
                throw;
            }
        }
        throw AppError("HTTP Range resume failed after partial reset");
    }

    DWORD get_discard(const std::wstring& webdav_relative) {
        mmid::check_cancelled(cancel_);
        CatalogLocation location = resolve_catalog_location(cfg_, webdav_relative);
        if (location.transport == SourceTransport::File) {
            fs::path source = location.file_path;
            reject_existing_reparse_points(source, "Catalog source");
            if (!fs::exists(source)) {
                throw AppError("Catalog source not found: " + printable_path(source));
            }
            if (!fs::is_regular_file(source)) {
                throw AppError("Catalog source is not a regular file: " + printable_path(source));
            }
            return 200;
        }
        std::lock_guard<std::mutex> transfer_lock(network_transfer_mutex());
        if (location.transport == SourceTransport::Ftp) {
            return request_ftp_url(location.url, nullptr, kMaxMetadataDownloadBytes);
        }
        return request_url(L"GET", location.url, nullptr, kMaxMetadataDownloadBytes);
    }

    DWORD get_absolute_discard(const std::wstring& absolute_path) {
        mmid::check_cancelled(cancel_);
        std::lock_guard<std::mutex> transfer_lock(network_transfer_mutex());
        if (looks_like_ftp_url(absolute_path)) {
            return request_ftp_url(absolute_path, nullptr, kMaxMetadataDownloadBytes);
        }
        if (looks_like_url(absolute_path)) {
            return request_url(L"GET", absolute_path, nullptr, kMaxMetadataDownloadBytes);
        }
        return request(L"GET", absolute_path, nullptr, kMaxMetadataDownloadBytes);
    }

private:
    void check_cancelled() const {
        mmid::check_cancelled(cancel_);
    }

    [[noreturn]] static void throw_winhttp_diagnostic(const char* operation,
        DWORD error,
        const ParsedHttpUrl& parsed) {
        (void)parsed;
        std::ostringstream message;
        message << operation << " failed: " << format_winhttp_error(error);
        throw AppError(message.str());
    }

    static void require_winhttp_context(bool ok, const char* operation, const ParsedHttpUrl& parsed) {
        if (!ok) {
            throw_winhttp_diagnostic(operation, GetLastError(), parsed);
        }
    }

    struct ParsedFtpUrl {
        std::wstring host;
        INTERNET_PORT port = 21;
        std::wstring path;
    };

    ParsedFtpUrl parse_ftp_url(const std::wstring& url) {
        std::wstring rest = trim_w(url);
        validate_url_has_no_userinfo(rest, "FTP URL");
        if (!starts_with_i(rest, L"ftp://")) {
            throw AppError("FTP URL must start with ftp://");
        }
        rest.erase(0, 6);

        size_t slash = rest.find(L'/');
        std::wstring authority = slash == std::wstring::npos ? rest : rest.substr(0, slash);
        std::wstring path = slash == std::wstring::npos ? L"/" : rest.substr(slash);
        if (authority.empty()) {
            throw AppError("FTP URL host is empty");
        }

        ParsedFtpUrl parsed;
        parsed.path = path.empty() ? L"/" : path;

        size_t at = authority.rfind(L'@');
        std::wstring host_port = authority;
        if (at != std::wstring::npos) {
            throw AppError("FTP URL must not contain userinfo or embedded credentials");
        }

        if (host_port.empty()) {
            throw AppError("FTP URL host is empty");
        }
        if (host_port.front() == L'[') {
            size_t close = host_port.find(L']');
            if (close == std::wstring::npos) {
                throw AppError("Invalid FTP IPv6 host");
            }
            parsed.host = host_port.substr(1, close - 1);
            if (close + 1 < host_port.size()) {
                if (host_port[close + 1] != L':') {
                    throw AppError("Invalid FTP host/port");
                }
                std::wstring port_text = host_port.substr(close + 2);
                if (!port_text.empty()) {
                    uint64_t port = parse_uint64_decimal_strict(port_text, "FTP port");
                    if (port == 0 || port > (std::numeric_limits<INTERNET_PORT>::max)()) {
                        throw AppError("FTP port is out of range");
                    }
                    parsed.port = static_cast<INTERNET_PORT>(port);
                }
            }
        } else {
            size_t colon = host_port.rfind(L':');
            if (colon != std::wstring::npos && colon + 1 < host_port.size()) {
                std::wstring port_text = host_port.substr(colon + 1);
                bool numeric = std::all_of(port_text.begin(), port_text.end(), [](wchar_t c) { return iswdigit(c) != 0; });
                if (numeric) {
                    parsed.host = host_port.substr(0, colon);
                    uint64_t port = parse_uint64_decimal_strict(port_text, "FTP port");
                    if (port == 0 || port > (std::numeric_limits<INTERNET_PORT>::max)()) {
                        throw AppError("FTP port is out of range");
                    }
                    parsed.port = static_cast<INTERNET_PORT>(port);
                } else {
                    parsed.host = host_port;
                }
            } else {
                parsed.host = host_port;
            }
        }

        if (parsed.host.empty()) {
            throw AppError("FTP URL host is empty");
        }
        return parsed;
    }

    DWORD request_ftp_url(const std::wstring& url, std::ofstream* output, uint64_t max_bytes) {
        check_cancelled();
        validate_credentials_for_transport(cfg_, false, "FTP");
        ParsedFtpUrl parsed = parse_ftp_url(url);

        std::wstring user_agent = app_user_agent();
        WinInetHandle session(InternetOpenW(user_agent.c_str(),
            kInternetOpenTypePreconfig,
            nullptr,
            nullptr,
            0));
        require_wininet(static_cast<bool>(session), "InternetOpen");
        DWORD connect_timeout = 15000;
        DWORD transfer_timeout = 30000;
        require_wininet(InternetSetOptionW(session.value, kInternetOptionConnectTimeout, &connect_timeout, sizeof(connect_timeout)),
            "InternetSetOption(CONNECT_TIMEOUT)");
        require_wininet(InternetSetOptionW(session.value, kInternetOptionSendTimeout, &transfer_timeout, sizeof(transfer_timeout)),
            "InternetSetOption(SEND_TIMEOUT)");
        require_wininet(InternetSetOptionW(session.value, kInternetOptionReceiveTimeout, &transfer_timeout, sizeof(transfer_timeout)),
            "InternetSetOption(RECEIVE_TIMEOUT)");

        auto connect = [&](DWORD flags, const char* operation) {
            WinInetHandle connection(InternetConnectW(session.value,
                parsed.host.c_str(),
                parsed.port,
                nullptr,
                nullptr,
                kInternetServiceFtp,
                flags,
                0));
            require_wininet(static_cast<bool>(connection), operation);
            return connection;
        };

        DWORD flags = kFtpTransferTypeBinary | kInternetFlagReload | kInternetFlagNoCacheWrite;

        auto open_file = [&](bool passive, std::string& error_message) {
            try {
                WinInetHandle connection = connect(passive ? kInternetFlagPassive : 0,
                    passive ? "InternetConnect(FTP passive)" : "InternetConnect(FTP active)");
                WinInetHandle file(FtpOpenFileW(connection.value,
                    parsed.path.c_str(),
                    GENERIC_READ,
                    flags,
                    0));
                if (!file) {
                    error_message = format_wininet_error(GetLastError());
                    return std::pair<WinInetHandle, WinInetHandle>{WinInetHandle{}, WinInetHandle{}};
                }
                return std::pair<WinInetHandle, WinInetHandle>{std::move(connection), std::move(file)};
            } catch (const std::exception& ex) {
                error_message = ex.what();
                return std::pair<WinInetHandle, WinInetHandle>{WinInetHandle{}, WinInetHandle{}};
            }
        };

        std::string passive_message;
        std::string active_message;
        WinInetHandle connection;
        WinInetHandle file;

        if (cfg_.ftp_mode == FtpMode::Passive || cfg_.ftp_mode == FtpMode::Auto) {
            check_cancelled();
            auto opened = open_file(true, passive_message);
            connection = std::move(opened.first);
            file = std::move(opened.second);
        }
        if (!file && (cfg_.ftp_mode == FtpMode::Active || cfg_.ftp_mode == FtpMode::Auto)) {
            check_cancelled();
            auto opened = open_file(false, active_message);
            connection = std::move(opened.first);
            file = std::move(opened.second);
        }

        if (!file) {
            if (cfg_.ftp_mode == FtpMode::Passive) {
                throw AppError("FtpOpenFile failed: passive mode failed: " + passive_message);
            }
            if (cfg_.ftp_mode == FtpMode::Active) {
                throw AppError("FtpOpenFile failed: active mode failed: " + active_message);
            }
            throw AppError("FtpOpenFile failed: passive mode failed: " + passive_message +
                "; active mode failed: " + active_message);
        }

        std::vector<char> buffer(64 * 1024);
        uint64_t total_received = 0;
        for (;;) {
            check_cancelled();
            DWORD read = 0;
            require_wininet(InternetReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read), "InternetReadFile(FTP)");
            if (read == 0) {
                break;
            }
            if (read > max_bytes - total_received) {
                throw AppError("FTP response exceeds configured download byte limit");
            }
            total_received += read;
            check_cancelled();
            if (output) {
                output->write(buffer.data(), read);
                if (!*output) {
                    throw AppError("Could not write FTP response to target file");
                }
            }
        }
        return 200;
    }

    struct HttpAttemptResult {
        DWORD status = 0;
        std::optional<std::wstring> redirect_location;
    };

    static DWORD query_http_status(HINTERNET request_handle) {
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        require_win32(WinHttpQueryHeaders(request_handle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &status_size,
            WINHTTP_NO_HEADER_INDEX),
            "WinHttpQueryHeaders(status)");
        return status;
    }

    static std::optional<std::wstring> query_http_header(HINTERNET request_handle, DWORD query) {
        DWORD bytes = 0;
        if (WinHttpQueryHeaders(request_handle,
                query,
                WINHTTP_HEADER_NAME_BY_INDEX,
                nullptr,
                &bytes,
                WINHTTP_NO_HEADER_INDEX)) {
            return std::wstring{};
        }
        DWORD error = GetLastError();
        if (error == ERROR_WINHTTP_HEADER_NOT_FOUND) {
            return std::nullopt;
        }
        if (error != ERROR_INSUFFICIENT_BUFFER) {
            throw AppError("WinHttpQueryHeaders failed: " + format_win32_error(error));
        }
        std::vector<wchar_t> buffer(static_cast<size_t>(bytes / sizeof(wchar_t)) + 1);
        require_win32(WinHttpQueryHeaders(request_handle,
            query,
            WINHTTP_HEADER_NAME_BY_INDEX,
            buffer.data(),
            &bytes,
            WINHTTP_NO_HEADER_INDEX),
            "WinHttpQueryHeaders(value)");
        size_t characters = bytes / sizeof(wchar_t);
        while (characters > 0 && buffer[characters - 1] == L'\0') {
            --characters;
        }
        return trim_w(std::wstring(buffer.data(), characters));
    }

    static bool is_supported_redirect_status(DWORD status) {
        return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    }

    HttpAttemptResult request_host_path(const std::wstring& method,
        const ParsedHttpUrl& parsed,
        std::ofstream* output,
        uint64_t max_bytes,
        uint64_t resume_offset = 0) {
        check_cancelled();
        validate_credentials_for_transport(cfg_, parsed.https, "HTTP");

        std::wstring user_agent = app_user_agent();
        WinHttpHandle session(WinHttpOpen(user_agent.c_str(),
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0));
        require_winhttp_context(static_cast<bool>(session), "WinHttpOpen", parsed);
        require_winhttp_context(WinHttpSetTimeouts(session.value, 15000, 15000, 30000, 60000), "WinHttpSetTimeouts", parsed);

        DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        require_winhttp_context(WinHttpSetOption(session.value,
            WINHTTP_OPTION_REDIRECT_POLICY,
            &redirect_policy,
            sizeof(redirect_policy)),
            "WinHttpSetOption(REDIRECT_POLICY_NEVER)",
            parsed);
        DWORD auto_logon_policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        require_winhttp_context(WinHttpSetOption(session.value,
            WINHTTP_OPTION_AUTOLOGON_POLICY,
            &auto_logon_policy,
            sizeof(auto_logon_policy)),
            "WinHttpSetOption(AUTOLOGON_POLICY_HIGH)",
            parsed);
        DWORD max_header_size = kMaxHttpResponseHeaderBytes;
        require_winhttp_context(WinHttpSetOption(session.value,
            WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE,
            &max_header_size,
            sizeof(max_header_size)),
            "WinHttpSetOption(MAX_RESPONSE_HEADER_SIZE)",
            parsed);
        if (parsed.https) {
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            require_winhttp_context(WinHttpSetOption(session.value,
                WINHTTP_OPTION_SECURE_PROTOCOLS,
                &protocols,
                sizeof(protocols)),
                "WinHttpSetOption(TLS1_2)",
                parsed);
        }

        WinHttpHandle connection(WinHttpConnect(session.value, parsed.host.c_str(), parsed.port, 0));
        require_winhttp_context(static_cast<bool>(connection), "WinHttpConnect", parsed);

        DWORD flags = parsed.https ? WINHTTP_FLAG_SECURE : 0;
        WinHttpHandle request_handle(WinHttpOpenRequest(connection.value,
            method.c_str(),
            parsed.path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags));
        require_winhttp_context(static_cast<bool>(request_handle), "WinHttpOpenRequest", parsed);
        if (parsed.https) {
            DWORD revocation = WINHTTP_ENABLE_SSL_REVOCATION;
            require_winhttp_context(WinHttpSetOption(request_handle.value,
                WINHTTP_OPTION_ENABLE_FEATURE,
                &revocation,
                sizeof(revocation)),
                "WinHttpSetOption(SSL_REVOCATION)",
                parsed);
        }
        if (resume_offset > 0) {
            std::wstring range_header = L"Range: bytes=" + std::to_wstring(resume_offset) + L"-\r\n";
            require_winhttp_context(WinHttpAddRequestHeaders(request_handle.value,
                range_header.c_str(),
                static_cast<DWORD>(-1L),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE),
                "WinHttpAddRequestHeaders(Range)",
                parsed);
        }

        auto send_receive = [&]() {
            check_cancelled();
            require_winhttp_context(WinHttpSendRequest(request_handle.value,
                WINHTTP_NO_ADDITIONAL_HEADERS,
                0,
                WINHTTP_NO_REQUEST_DATA,
                0,
                0,
                0),
                "WinHttpSendRequest",
                parsed);
            require_winhttp_context(WinHttpReceiveResponse(request_handle.value, nullptr), "WinHttpReceiveResponse", parsed);
            return query_http_status(request_handle.value);
        };

        DWORD status = send_receive();
        if (status == 401 && config_has_credentials(cfg_)) {
            if (!parsed.https) {
                throw AppError("HTTP authentication is forbidden without HTTPS");
            }
            DWORD supported_schemes = 0;
            DWORD first_scheme = 0;
            DWORD auth_target = 0;
            require_winhttp_context(WinHttpQueryAuthSchemes(request_handle.value,
                &supported_schemes,
                &first_scheme,
                &auth_target),
                "WinHttpQueryAuthSchemes",
                parsed);
            if (auth_target != WINHTTP_AUTH_TARGET_SERVER ||
                (supported_schemes & WINHTTP_AUTH_SCHEME_BASIC) == 0) {
                throw AppError("HTTPS server did not offer the required Basic authentication scheme");
            }
            SecureWideString password(unprotect_password(cfg_.password_dpapi));
            if (trim_w(cfg_.username).empty() || password.value().empty()) {
                throw AppError("Configured HTTPS credentials are incomplete");
            }
            require_winhttp_context(WinHttpSetCredentials(request_handle.value,
                WINHTTP_AUTH_TARGET_SERVER,
                WINHTTP_AUTH_SCHEME_BASIC,
                cfg_.username.c_str(),
                password.value().c_str(),
                nullptr),
                "WinHttpSetCredentials(Basic over HTTPS)",
                parsed);
            status = send_receive();
        }

        if (resume_offset > 0) {
            if (status == 200) {
                throw AppError("HTTP Range resume failed: server ignored Range request");
            }
            if (status == 416) {
                throw AppError("HTTP Range resume failed: server rejected resume offset");
            }
            if (status != 206 && !is_supported_redirect_status(status)) {
                throw AppError("HTTP status " + std::to_string(status));
            }
        }

        if (is_supported_redirect_status(status)) {
            auto location = query_http_header(request_handle.value, WINHTTP_QUERY_LOCATION);
            if (!location || location->empty()) {
                throw AppError("HTTP " + std::to_string(status) +
                    " redirect is missing a valid Location header");
            }
            return HttpAttemptResult{status, std::move(location)};
        }
        if (status >= 300 && status < 400) {
            throw AppError("Unsupported HTTP redirect status " + std::to_string(status));
        }
        if (status != 200) {
            if (!(resume_offset > 0 && status == 206)) {
                throw AppError("HTTP status " + std::to_string(status));
            }
        }

        std::optional<uint64_t> content_length;
        if (auto header = query_http_header(request_handle.value, WINHTTP_QUERY_CONTENT_LENGTH)) {
            content_length = parse_uint64_decimal_strict(*header, "HTTP Content-Length");
            if (*content_length > max_bytes || resume_offset > max_bytes - *content_length) {
                throw AppError("HTTP Content-Length exceeds configured download byte limit");
            }
        }

        std::vector<char> buffer(64 * 1024);
        uint64_t total_received = 0;
        for (;;) {
            check_cancelled();
            DWORD available = 0;
            require_winhttp_context(WinHttpQueryDataAvailable(request_handle.value, &available), "WinHttpQueryDataAvailable", parsed);
            if (available == 0) {
                break;
            }
            DWORD remaining = available;
            while (remaining > 0) {
                check_cancelled();
                DWORD to_read = std::min<DWORD>(remaining, static_cast<DWORD>(buffer.size()));
                DWORD read = 0;
                require_winhttp_context(WinHttpReadData(request_handle.value, buffer.data(), to_read, &read), "WinHttpReadData", parsed);
                if (read == 0) {
                    remaining = 0;
                    break;
                }
                if (resume_offset + total_received > max_bytes ||
                    read > max_bytes - resume_offset - total_received) {
                    throw AppError("HTTP response exceeds configured download byte limit");
                }
                total_received += read;
                if (output) {
                    output->write(buffer.data(), read);
                    if (!*output) {
                        throw AppError("Could not write HTTP response to target file");
                    }
                }
                remaining -= read;
            }
        }
        if (content_length && total_received != *content_length) {
            throw AppError("HTTP response length does not match Content-Length");
        }
        return HttpAttemptResult{status, std::nullopt};
    }

    DWORD request_url(const std::wstring& method,
        const std::wstring& url,
        std::ofstream* output,
        uint64_t max_bytes,
        uint64_t resume_offset = 0) {
        std::wstring current_url = trim_w(url);
        ParsedHttpUrl current = parse_http_url_strict(current_url);
        for (size_t redirects = 0;; ++redirects) {
            HttpAttemptResult result = request_host_path(method, current, output, max_bytes, resume_offset);
            if (!result.redirect_location) {
                return result.status;
            }
            if (redirects >= kMaxHttpsRedirects) {
                throw AppError("HTTP redirect limit exceeded");
            }
            std::wstring next_url = combine_redirect_url(current_url, *result.redirect_location);
            ParsedHttpUrl next = parse_http_url_strict(next_url);
            if (!same_https_origin(current, next)) {
                throw AppError("HTTP redirect must remain on the same HTTPS origin without downgrade");
            }
            current_url = std::move(next_url);
            current = std::move(next);
        }
    }

    DWORD request(const std::wstring& method,
        const std::wstring& path,
        std::ofstream* output,
        uint64_t max_bytes) {
        check_cancelled();
        validate_server_has_no_userinfo(cfg_.server, "WebDAV server");
        std::wstring host = normalize_server(cfg_.server);
        if (host.empty()) {
            throw AppError("Server is empty in config");
        }
        std::wstring url = bool_to_transport(cfg_.https) + L"://" + host;
        if (path.empty() || path.front() != L'/') {
            url.push_back(L'/');
        }
        url += path;
        return request_url(method, url, output, max_bytes);
    }

    Config cfg_;
    CancellationCallback cancel_;
};

DWORD probe_catalog_source(
    const Config& cfg,
    const std::wstring& catalog_relative,
    const CancellationCallback& cancel) {
    WinHttpClient client(cfg, cancel);
    return client.get_discard(catalog_relative);
}

void validate_package_info(const PackageInfo& package, const fs::path& package_xml);

struct ZipEntryInfo {
    std::string name;
    uint16_t flags = 0;
    uint16_t method = 0;
    uint16_t aes_vendor_version = 0;
    uint16_t aes_actual_method = 0;
    uint8_t aes_strength = 0;
    uint32_t crc32 = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint16_t disk_number_start = 0;
    uint32_t local_header_offset = 0;
};

struct ZipPart {
    fs::path path;
    uint64_t start = 0;
    uint64_t size = 0;
};

struct ZipArchiveView {
    std::vector<ZipPart> parts;
    uint64_t total_size = 0;
};

uint16_t le16(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 2 > data.size()) {
        throw AppError("Unexpected end of ZIP data");
    }
    return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
}

uint32_t le32(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) {
        throw AppError("Unexpected end of ZIP data");
    }
    return static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1]) << 8) |
        (static_cast<uint32_t>(data[offset + 2]) << 16) |
        (static_cast<uint32_t>(data[offset + 3]) << 24);
}

std::vector<uint8_t> read_file_range(const fs::path& path, uint64_t offset, size_t size) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw AppError("Could not open file: " + printable_path(path));
    }
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        throw AppError("Could not seek file: " + printable_path(path));
    }
    std::vector<uint8_t> data(size);
    input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(input.gcount()));
    if (data.size() != size) {
        throw AppError("Could not read requested file range: " + printable_path(path));
    }
    return data;
}

ZipArchiveView open_zip_archive_view(const fs::path& zip_path) {
    ZipArchiveView view;
    fs::path directory = zip_path.parent_path();
    std::wstring stem = zip_path.stem().wstring();
    auto append_part = [&](const fs::path& part) {
        reject_existing_reparse_points(part, "ZIP input path");
        std::error_code ec;
        if (!fs::is_regular_file(part, ec) || ec) {
            throw AppError("ZIP input is not an ordinary regular file: " + printable_path(part) +
                (ec ? ": " + ec.message() : ""));
        }
        uintmax_t part_size = fs::file_size(part, ec);
        if (ec) {
            throw AppError("Could not read ZIP input size: " + printable_path(part) + ": " + ec.message());
        }
        if (part_size > (std::numeric_limits<uint64_t>::max)() - view.total_size) {
            throw AppError("Split ZIP total size is out of range");
        }
        view.parts.push_back(ZipPart{part, view.total_size, static_cast<uint64_t>(part_size)});
        view.total_size += static_cast<uint64_t>(part_size);
    };

    std::wstring extension = zip_path.extension().wstring();
    if (extension.size() == 4 &&
        iswdigit(extension[1]) &&
        iswdigit(extension[2]) &&
        iswdigit(extension[3]) &&
        iequals_w(fs::path(stem).extension().wstring(), L".zip")) {
        fs::path base = directory / stem;
        for (int i = 1; i <= 999; ++i) {
            wchar_t ext[8]{};
            swprintf_s(ext, L".%03d", i);
            fs::path part = base;
            part += ext;
            if (fs::exists(part)) {
                append_part(part);
            } else if (!view.parts.empty()) {
                break;
            }
        }
        if (!view.parts.empty()) {
            return view;
        }
    }

    for (int i = 1; i <= (std::numeric_limits<uint16_t>::max)(); ++i) {
        wchar_t ext[8]{};
        swprintf_s(ext, L".z%02d", i);
        fs::path part = directory / (stem + ext);
        if (fs::exists(part)) {
            append_part(part);
        } else {
            break;
        }
    }

    reject_existing_reparse_points(zip_path, "ZIP input path");
    if (!fs::exists(zip_path)) {
        throw AppError("ZIP file not found: " + printable_path(zip_path));
    }
    append_part(zip_path);
    return view;
}

std::vector<uint8_t> read_zip_absolute_range(const ZipArchiveView& view, uint64_t absolute_offset, size_t size) {
    if (absolute_offset + size > view.total_size) {
        throw AppError("ZIP range exceeds archive size");
    }

    std::vector<uint8_t> output;
    output.reserve(size);
    uint64_t current = absolute_offset;
    size_t remaining = size;

    while (remaining > 0) {
        auto part_it = std::find_if(view.parts.begin(), view.parts.end(), [&](const ZipPart& part) {
            return current >= part.start && current < part.start + part.size;
        });
        if (part_it == view.parts.end()) {
            throw AppError("ZIP range does not map to a split part");
        }

        uint64_t offset_in_part = current - part_it->start;
        size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, part_it->size - offset_in_part));
        std::vector<uint8_t> bytes = read_file_range(part_it->path, offset_in_part, chunk);
        output.insert(output.end(), bytes.begin(), bytes.end());
        current += chunk;
        remaining -= chunk;
    }
    return output;
}

std::vector<uint8_t> read_zip_disk_range(const ZipArchiveView& view,
    uint16_t disk_number,
    uint64_t offset,
    size_t size) {
    if (disk_number >= view.parts.size()) {
        throw AppError("ZIP disk number is outside available split parts");
    }
    return read_zip_absolute_range(view, view.parts[disk_number].start + offset, size);
}

const char* zip_method_name(uint16_t method) {
    switch (method) {
    case 0: return "stored";
    case 8: return "deflate";
    case 12: return "bzip2";
    case 99: return "winzip-aes";
    default: return "unknown";
    }
}

void parse_zip_extra(const std::vector<uint8_t>& extra, ZipEntryInfo& entry) {
    size_t offset = 0;
    while (offset + 4 <= extra.size()) {
        uint16_t header_id = le16(extra, offset);
        uint16_t size = le16(extra, offset + 2);
        offset += 4;
        if (offset + size > extra.size()) {
            break;
        }
        if (header_id == 0x9901 && size >= 7) {
            entry.aes_vendor_version = static_cast<uint16_t>(extra[offset] | (extra[offset + 1] << 8));
            entry.aes_strength = extra[offset + 4];
            entry.aes_actual_method = static_cast<uint16_t>(extra[offset + 5] | (extra[offset + 6] << 8));
        }
        offset += size;
    }
}

std::vector<ZipEntryInfo> read_zip_central_directory(const fs::path& path) {
    ZipArchiveView view = open_zip_archive_view(path);
    size_t tail_size = static_cast<size_t>(std::min<uint64_t>(view.total_size, 22 + 65535));
    std::vector<uint8_t> tail = read_zip_absolute_range(view, view.total_size - tail_size, tail_size);

    size_t eocd = std::string::npos;
    for (size_t i = tail.size(); i >= 4; --i) {
        size_t pos = i - 4;
        if (tail[pos] == 0x50 && tail[pos + 1] == 0x4B && tail[pos + 2] == 0x05 && tail[pos + 3] == 0x06) {
            eocd = pos;
            break;
        }
    }
    if (eocd == std::string::npos) {
        throw AppError("ZIP end-of-central-directory record not found");
    }
    if (eocd + 22 > tail.size()) {
        throw AppError("Truncated ZIP end-of-central-directory record");
    }

    uint16_t disk_number = le16(tail, eocd + 4);
    uint16_t cd_disk = le16(tail, eocd + 6);
    uint16_t entry_count = le16(tail, eocd + 10);
    uint32_t cd_size = le32(tail, eocd + 12);
    uint32_t cd_offset = le32(tail, eocd + 16);

    if (disk_number >= view.parts.size() || cd_disk >= view.parts.size()) {
        throw AppError("Split ZIP disk number is outside available parts");
    }
    if (entry_count == 0xFFFF || cd_size == 0xFFFFFFFF || cd_offset == 0xFFFFFFFF) {
        throw AppError("ZIP64 central directory is not implemented yet");
    }
    if (entry_count > kMaxZipCentralEntries) {
        throw AppError("ZIP central directory contains too many entries");
    }
    if (cd_size > kMaxZipCentralDirectoryBytes) {
        throw AppError("ZIP central directory exceeds the memory safety limit");
    }

    std::vector<uint8_t> cd = read_zip_disk_range(view, cd_disk, cd_offset, cd_size);
    std::vector<ZipEntryInfo> entries;
    entries.reserve(entry_count);
    size_t explicit_directories = 0;
    size_t offset = 0;
    while (offset + 46 <= cd.size()) {
        if (le32(cd, offset) != 0x02014b50) {
            throw AppError("Invalid ZIP central directory header");
        }
        ZipEntryInfo entry;
        entry.flags = le16(cd, offset + 8);
        entry.method = le16(cd, offset + 10);
        entry.crc32 = le32(cd, offset + 16);
        entry.compressed_size = le32(cd, offset + 20);
        entry.uncompressed_size = le32(cd, offset + 24);
        entry.disk_number_start = le16(cd, offset + 34);
        entry.local_header_offset = le32(cd, offset + 42);
        uint16_t name_len = le16(cd, offset + 28);
        uint16_t extra_len = le16(cd, offset + 30);
        uint16_t comment_len = le16(cd, offset + 32);
        size_t variable = static_cast<size_t>(name_len) + extra_len + comment_len;
        if (offset + 46 + variable > cd.size()) {
            throw AppError("Truncated ZIP central directory entry");
        }
        entry.name.assign(reinterpret_cast<const char*>(cd.data() + offset + 46), name_len);
        if (!entry.name.empty() && (entry.name.back() == '/' || entry.name.back() == '\\')) {
            ++explicit_directories;
            if (explicit_directories > kMaxZipExplicitDirectories) {
                throw AppError("ZIP central directory contains too many explicit directories");
            }
        }
        std::vector<uint8_t> extra(cd.begin() + offset + 46 + name_len,
            cd.begin() + offset + 46 + name_len + extra_len);
        parse_zip_extra(extra, entry);
        entries.push_back(std::move(entry));
        offset += 46 + variable;
    }
    if (entries.size() != entry_count) {
        throw AppError("ZIP entry count mismatch");
    }
    return entries;
}

uint32_t zip_crc32_update(uint32_t crc, uint8_t value) {
    static uint32_t table[256]{};
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = true;
    }
    return table[(crc ^ value) & 0xFF] ^ (crc >> 8);
}

uint32_t zip_crc32(const std::vector<uint8_t>& bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t value : bytes) {
        crc = zip_crc32_update(crc, value);
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t zip_crc32_update_block(uint32_t crc, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        crc = zip_crc32_update(crc, data[i]);
    }
    return crc;
}


class ZipCrypto {
public:
    explicit ZipCrypto(const std::string& password) {
        for (uint8_t value : password) {
            update_keys(value);
        }
    }

    uint8_t decrypt(uint8_t value) {
        uint8_t plain = static_cast<uint8_t>(value ^ decrypt_byte());
        update_keys(plain);
        return plain;
    }

private:
    void update_keys(uint8_t value) {
        keys_[0] = zip_crc32_update(keys_[0], value);
        keys_[1] = keys_[1] + (keys_[0] & 0xFF);
        keys_[1] = keys_[1] * 134775813u + 1u;
        keys_[2] = zip_crc32_update(keys_[2], static_cast<uint8_t>(keys_[1] >> 24));
    }

    uint8_t decrypt_byte() const {
        uint16_t temp = static_cast<uint16_t>((keys_[2] & 0xFFFF) | 2);
        return static_cast<uint8_t>(((temp * (temp ^ 1)) >> 8) & 0xFF);
    }

    uint32_t keys_[3] = {305419896u, 591751049u, 878082192u};
};

bool zip_entry_is_directory(const ZipEntryInfo& entry) {
    return !entry.name.empty() && (entry.name.back() == '/' || entry.name.back() == '\\');
}

fs::path safe_zip_relative_path(const std::string& raw_name) {
    if (raw_name.empty()) {
        throw AppError("Unsafe ZIP path is empty");
    }
    std::string normalized = raw_name;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.front() == '/' || normalized.find("//") != std::string::npos) {
        throw AppError("Unsafe ZIP path is rooted or contains an empty component");
    }

    fs::path result;
    const bool directory_marker = normalized.back() == '/';
    const size_t logical_size = normalized.size() - (directory_marker ? 1u : 0u);
    if (logical_size == 0) {
        throw AppError("Unsafe ZIP path has no usable components");
    }
    size_t start = 0;
    while (start < logical_size) {
        size_t pos = normalized.find('/', start);
        if (pos == std::string::npos || pos > logical_size) {
            pos = logical_size;
        }
        std::string part = normalized.substr(start, pos - start);
        if (part.empty()) {
            throw AppError("Unsafe ZIP path contains an empty component");
        }
        if (part == ".") {
            if (pos == logical_size) {
                break;
            }
            start = pos + 1;
            continue;
        }
        std::wstring component;
        try {
            component = utf8_to_wide(part);
        } catch (const std::exception&) {
            component.assign(part.begin(), part.end());
        }
        validate_safe_path_component(component, "ZIP path component");
        result /= component;
        if (pos == logical_size) {
            break;
        }
        start = pos + 1;
    }
    if (result.empty() || result.has_root_name() || result.has_root_directory() || result.is_absolute()) {
        throw AppError("Unsafe ZIP path is not relative");
    }
    return result;
}

using ByteChunkCallback = std::function<void(const uint8_t* data, size_t size)>;

uint64_t zip_entry_data_absolute_offset(const ZipArchiveView& view, const ZipEntryInfo& entry) {
    std::vector<uint8_t> local = read_zip_disk_range(view, entry.disk_number_start, entry.local_header_offset, 30);
    if (le32(local, 0) != 0x04034b50) {
        throw AppError("Invalid ZIP local file header for entry: " + entry.name);
    }
    uint16_t name_len = le16(local, 26);
    uint16_t extra_len = le16(local, 28);
    return view.parts[entry.disk_number_start].start +
        static_cast<uint64_t>(entry.local_header_offset) +
        30u +
        name_len +
        extra_len;
}

void stream_zip_absolute_range(const ZipArchiveView& view,
    uint64_t absolute_offset,
    uint64_t size,
    const ByteChunkCallback& chunk,
    const CancellationCallback& cancel = {}) {
    if (absolute_offset + size > view.total_size) {
        throw AppError("ZIP range exceeds archive size");
    }
    constexpr size_t buffer_size = 256 * 1024;
    std::vector<uint8_t> buffer(buffer_size);
    uint64_t current = absolute_offset;
    uint64_t remaining = size;
    while (remaining > 0) {
        check_cancelled(cancel);
        auto part_it = std::find_if(view.parts.begin(), view.parts.end(), [&](const ZipPart& part) {
            return current >= part.start && current < part.start + part.size;
        });
        if (part_it == view.parts.end()) {
            throw AppError("ZIP range does not map to a split part");
        }

        uint64_t offset_in_part = current - part_it->start;
        uint64_t available_in_part = part_it->size - offset_in_part;
        uint64_t part_remaining = std::min<uint64_t>(remaining, available_in_part);
        std::ifstream input(part_it->path, std::ios::binary);
        if (!input) {
            throw AppError("Could not open file: " + printable_path(part_it->path));
        }
        input.seekg(static_cast<std::streamoff>(offset_in_part), std::ios::beg);
        if (!input) {
            throw AppError("Could not seek file: " + printable_path(part_it->path));
        }
        while (part_remaining > 0) {
            check_cancelled(cancel);
            size_t to_read = static_cast<size_t>(std::min<uint64_t>(buffer.size(), part_remaining));
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(to_read));
            size_t got = static_cast<size_t>(input.gcount());
            if (got != to_read) {
                throw AppError("Could not read requested file range: " + printable_path(part_it->path));
            }
            chunk(buffer.data(), got);
            current += got;
            remaining -= got;
            part_remaining -= got;
        }
    }
}

std::vector<uint8_t> derive_pbkdf2_sha1(const std::string& password,
    const std::vector<uint8_t>& salt,
    size_t output_size) {
    BCryptAlgHandle sha1;
    require_ntstatus(BCryptOpenAlgorithmProvider(&sha1.value, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
        "BCryptOpenAlgorithmProvider(SHA1 HMAC)");

    std::vector<uint8_t> output(output_size);
    require_ntstatus(BCryptDeriveKeyPBKDF2(sha1.value,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt.data()),
        static_cast<ULONG>(salt.size()),
        1000,
        output.data(),
        static_cast<ULONG>(output.size()),
        0),
        "BCryptDeriveKeyPBKDF2");
    return output;
}

struct BCryptHashHandle {
    BCRYPT_HASH_HANDLE value = nullptr;
    ~BCryptHashHandle() {
        if (value) {
            BCryptDestroyHash(value);
        }
    }
};

std::vector<uint8_t> hmac_sha1(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data) {
    BCryptAlgHandle sha1;
    require_ntstatus(BCryptOpenAlgorithmProvider(&sha1.value, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
        "BCryptOpenAlgorithmProvider(HMAC SHA1)");

    DWORD object_length = 0;
    DWORD written = 0;
    require_ntstatus(BCryptGetProperty(sha1.value,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_length),
        sizeof(object_length),
        &written,
        0),
        "BCryptGetProperty(BCRYPT_OBJECT_LENGTH)");

    std::vector<uint8_t> object(object_length);
    BCryptHashHandle hash;
    require_ntstatus(BCryptCreateHash(sha1.value,
        &hash.value,
        object.data(),
        static_cast<ULONG>(object.size()),
        const_cast<PUCHAR>(key.data()),
        static_cast<ULONG>(key.size()),
        0),
        "BCryptCreateHash(HMAC SHA1)");
    require_ntstatus(BCryptHashData(hash.value,
        const_cast<PUCHAR>(data.data()),
        static_cast<ULONG>(data.size()),
        0),
        "BCryptHashData(HMAC SHA1)");

    std::vector<uint8_t> digest(20);
    require_ntstatus(BCryptFinishHash(hash.value,
        digest.data(),
        static_cast<ULONG>(digest.size()),
        0),
        "BCryptFinishHash(HMAC SHA1)");
    return digest;
}

class HmacSha1Stream {
public:
    explicit HmacSha1Stream(const std::vector<uint8_t>& key) {
        require_ntstatus(BCryptOpenAlgorithmProvider(&alg_.value, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
            "BCryptOpenAlgorithmProvider(HMAC SHA1)");

        DWORD object_length = 0;
        DWORD written = 0;
        require_ntstatus(BCryptGetProperty(alg_.value,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_length),
            sizeof(object_length),
            &written,
            0),
            "BCryptGetProperty(BCRYPT_OBJECT_LENGTH)");

        object_.resize(object_length);
        require_ntstatus(BCryptCreateHash(alg_.value,
            &hash_.value,
            object_.data(),
            static_cast<ULONG>(object_.size()),
            const_cast<PUCHAR>(key.data()),
            static_cast<ULONG>(key.size()),
            0),
            "BCryptCreateHash(HMAC SHA1)");
    }

    void update(const uint8_t* data, size_t size) {
        if (size == 0) {
            return;
        }
        require_ntstatus(BCryptHashData(hash_.value,
            const_cast<PUCHAR>(data),
            static_cast<ULONG>(size),
            0),
            "BCryptHashData(HMAC SHA1)");
    }

    std::vector<uint8_t> finish() {
        std::vector<uint8_t> digest(20);
        require_ntstatus(BCryptFinishHash(hash_.value,
            digest.data(),
            static_cast<ULONG>(digest.size()),
            0),
            "BCryptFinishHash(HMAC SHA1)");
        return digest;
    }

private:
    BCryptAlgHandle alg_;
    std::vector<uint8_t> object_;
    BCryptHashHandle hash_;
};

class AesEcbEncryptor {
public:
    explicit AesEcbEncryptor(const std::vector<uint8_t>& key) {
        require_ntstatus(BCryptOpenAlgorithmProvider(&alg_.value, BCRYPT_AES_ALGORITHM, nullptr, 0),
            "BCryptOpenAlgorithmProvider(AES)");
        require_ntstatus(BCryptSetProperty(alg_.value,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
            static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_ECB) + 1) * sizeof(wchar_t)),
            0),
            "BCryptSetProperty(AES ECB)");
        require_ntstatus(BCryptGenerateSymmetricKey(alg_.value,
            &key_.value,
            nullptr,
            0,
            const_cast<PUCHAR>(key.data()),
            static_cast<ULONG>(key.size()),
            0),
            "BCryptGenerateSymmetricKey(AES)");
    }

    std::array<uint8_t, 16> encrypt_block(const std::array<uint8_t, 16>& input) {
        std::array<uint8_t, 16> output{};
        ULONG output_size = 0;
        require_ntstatus(BCryptEncrypt(key_.value,
            const_cast<PUCHAR>(input.data()),
            static_cast<ULONG>(input.size()),
            nullptr,
            nullptr,
            0,
            output.data(),
            static_cast<ULONG>(output.size()),
            &output_size,
            0),
            "BCryptEncrypt(AES block)");
        if (output_size != output.size()) {
            throw AppError("AES block encryption returned unexpected size");
        }
        return output;
    }

private:
    BCryptAlgHandle alg_;
    BCryptKeyHandle key_;
};

void increment_winzip_counter(std::array<uint8_t, 16>& counter) {
    for (size_t i = 0; i < 16; ++i) {
        counter[i] = static_cast<uint8_t>(counter[i] + 1);
        if (counter[i] != 0) {
            break;
        }
    }
}

bool zip_entry_requires_crc(const ZipEntryInfo& entry) {
    return entry.method != 99 || entry.aes_vendor_version != 2;
}

void extract_zip_entry_to_file_streaming(const fs::path& zip_path,
    const ZipEntryInfo& entry,
    const fs::path& target,
    const std::wstring& password,
    const CancellationCallback& cancel = {}) {
    ZipArchiveView view = open_zip_archive_view(zip_path);
    uint64_t data_offset = zip_entry_data_absolute_offset(view, entry);
    uint16_t actual_method = entry.method == 99 ? entry.aes_actual_method : entry.method;
    if (actual_method != 0 && actual_method != 8 && actual_method != 12) {
        throw AppError("Unsupported ZIP method " + std::to_string(actual_method) +
            " (" + zip_method_name(actual_method) + ") for entry: " + entry.name);
    }

    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw AppError("Could not create extracted file: " + printable_path(target));
    }

    uint32_t crc = 0xFFFFFFFFu;
    uint64_t plain_written = 0;
    auto write_plain = [&](const uint8_t* data, size_t size) {
        if (size == 0) {
            return;
        }
        if (plain_written > entry.uncompressed_size ||
            size > static_cast<uint64_t>(entry.uncompressed_size) - plain_written) {
            throw AppError("ZIP entry exceeds declared uncompressed size before write: " + entry.name);
        }
        output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!output) {
            throw AppError("Could not write extracted file: " + printable_path(target));
        }
        crc = zip_crc32_update_block(crc, data, size);
        plain_written += size;
        check_cancelled(cancel);
    };

    constexpr size_t out_buffer_size = 256 * 1024;
    std::vector<uint8_t> out_buffer(out_buffer_size);
    z_stream zstream{};
    bool zstream_initialized = false;
    bool zstream_ended = false;
    bz_stream bzstream{};
    bool bzstream_initialized = false;
    bool bzstream_ended = false;

    if (actual_method == 8) {
        int rc = inflateInit2(&zstream, -MAX_WBITS);
        if (rc != Z_OK) {
            throw AppError("inflateInit2 failed: " + std::to_string(rc));
        }
        zstream_initialized = true;
    } else if (actual_method == 12) {
        int rc = BZ2_bzDecompressInit(&bzstream, 0, 0);
        if (rc != BZ_OK) {
            throw AppError("BZ2_bzDecompressInit failed: " + std::to_string(rc));
        }
        bzstream_initialized = true;
    }

    auto cleanup_streams = [&]() {
        if (zstream_initialized) {
            inflateEnd(&zstream);
            zstream_initialized = false;
        }
        if (bzstream_initialized) {
            BZ2_bzDecompressEnd(&bzstream);
            bzstream_initialized = false;
        }
    };

    auto feed_compressed = [&](const uint8_t* data, size_t size) {
        check_cancelled(cancel);
        if (size == 0) {
            return;
        }
        if (actual_method == 0) {
            write_plain(data, size);
            return;
        }
        if (actual_method == 8) {
            zstream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
            zstream.avail_in = static_cast<uInt>(size);
            while (zstream.avail_in > 0) {
                uInt input_before = zstream.avail_in;
                zstream.next_out = reinterpret_cast<Bytef*>(out_buffer.data());
                zstream.avail_out = static_cast<uInt>(out_buffer.size());
                int rc = inflate(&zstream, Z_NO_FLUSH);
                size_t produced = out_buffer.size() - zstream.avail_out;
                if (produced > 0) {
                    write_plain(out_buffer.data(), produced);
                }
                if (rc == Z_STREAM_END) {
                    zstream_ended = true;
                    if (zstream.avail_in != 0) {
                        throw AppError("inflate ended before compressed data was consumed: " + entry.name);
                    }
                    break;
                }
                if (rc != Z_OK) {
                    throw AppError("inflate failed: " + std::to_string(rc));
                }
                if (produced == 0 && zstream.avail_in == input_before) {
                    throw AppError("inflate made no progress: " + entry.name);
                }
            }
            return;
        }

        bzstream.next_in = const_cast<char*>(reinterpret_cast<const char*>(data));
        bzstream.avail_in = static_cast<unsigned int>(size);
        while (bzstream.avail_in > 0) {
            unsigned int input_before = bzstream.avail_in;
            bzstream.next_out = reinterpret_cast<char*>(out_buffer.data());
            bzstream.avail_out = static_cast<unsigned int>(out_buffer.size());
            int rc = BZ2_bzDecompress(&bzstream);
            size_t produced = out_buffer.size() - bzstream.avail_out;
            if (produced > 0) {
                write_plain(out_buffer.data(), produced);
            }
            if (rc == BZ_STREAM_END) {
                bzstream_ended = true;
                if (bzstream.avail_in != 0) {
                    throw AppError("bzip2 ended before compressed data was consumed: " + entry.name);
                }
                break;
            }
            if (rc != BZ_OK) {
                throw AppError("bzip2 decompress failed: " + std::to_string(rc));
            }
            if (produced == 0 && bzstream.avail_in == input_before) {
                throw AppError("bzip2 made no progress: " + entry.name);
            }
        }
    };

    auto finish_method = [&]() {
        if (actual_method == 8) {
            if (!zstream_ended) {
                for (;;) {
                    zstream.next_out = reinterpret_cast<Bytef*>(out_buffer.data());
                    zstream.avail_out = static_cast<uInt>(out_buffer.size());
                    int rc = inflate(&zstream, Z_FINISH);
                    size_t produced = out_buffer.size() - zstream.avail_out;
                    if (produced > 0) {
                        write_plain(out_buffer.data(), produced);
                    }
                    if (rc == Z_STREAM_END) {
                        zstream_ended = true;
                        break;
                    }
                    if (rc != Z_OK && rc != Z_BUF_ERROR) {
                        throw AppError("inflate finish failed: " + std::to_string(rc));
                    }
                    if (produced == 0) {
                        throw AppError("inflate stream ended unexpectedly: " + entry.name);
                    }
                }
            }
            if (zstream.total_out != entry.uncompressed_size) {
                throw AppError("inflate output size mismatch");
            }
        } else if (actual_method == 12) {
            if (!bzstream_ended) {
                throw AppError("bzip2 stream ended unexpectedly: " + entry.name);
            }
            uint64_t total_out = (static_cast<uint64_t>(bzstream.total_out_hi32) << 32) | bzstream.total_out_lo32;
            if (total_out != entry.uncompressed_size) {
                throw AppError("bzip2 output size mismatch");
            }
        }
    };

    try {
        if (entry.method == 99) {
            if (password.empty()) {
                throw AppError("AES ZIP entry is encrypted but package password is empty: " + entry.name);
            }
            size_t key_length = 0;
            size_t salt_length = 0;
            if (entry.aes_strength == 1) {
                key_length = 16;
                salt_length = 8;
            } else if (entry.aes_strength == 2) {
                key_length = 24;
                salt_length = 12;
            } else if (entry.aes_strength == 3) {
                key_length = 32;
                salt_length = 16;
            } else {
                throw AppError("Unsupported WinZip AES strength for entry: " + entry.name);
            }

            constexpr size_t verifier_length = 2;
            constexpr size_t auth_code_length = 10;
            size_t header_length = salt_length + verifier_length;
            if (entry.compressed_size < header_length + auth_code_length) {
                throw AppError("WinZip AES record is too short for entry: " + entry.name);
            }
            uint64_t encrypted_payload_size = entry.compressed_size - header_length - auth_code_length;
            std::vector<uint8_t> header;
            header.reserve(header_length);
            std::vector<uint8_t> auth_code;
            auth_code.reserve(auth_code_length);
            std::optional<AesEcbEncryptor> aes;
            std::optional<HmacSha1Stream> hmac;
            std::array<uint8_t, 16> counter{};
            std::array<uint8_t, 16> keystream{};
            size_t keystream_index = keystream.size();
            uint64_t encrypted_seen = 0;

            auto initialize_aes = [&]() {
                std::vector<uint8_t> salt(header.begin(), header.begin() + salt_length);
                std::vector<uint8_t> derived = derive_pbkdf2_sha1(wide_to_utf8(password), salt, key_length * 2 + verifier_length);
                if (derived[key_length * 2] != header[salt_length] ||
                    derived[key_length * 2 + 1] != header[salt_length + 1]) {
                    throw AppError("WinZip AES password verifier failed for entry: " + entry.name);
                }
                std::vector<uint8_t> encryption_key(derived.begin(), derived.begin() + key_length);
                std::vector<uint8_t> authentication_key(derived.begin() + key_length, derived.begin() + key_length * 2);
                aes.emplace(encryption_key);
                hmac.emplace(authentication_key);
            };

            auto decrypt_aes_payload = [&](const uint8_t* data, size_t size) {
                if (size == 0) {
                    return;
                }
                hmac->update(data, size);
                std::vector<uint8_t> plain(size);
                for (size_t i = 0; i < size; ++i) {
                    if (keystream_index >= keystream.size()) {
                        increment_winzip_counter(counter);
                        keystream = aes->encrypt_block(counter);
                        keystream_index = 0;
                    }
                    plain[i] = static_cast<uint8_t>(data[i] ^ keystream[keystream_index++]);
                }
                feed_compressed(plain.data(), plain.size());
            };

            stream_zip_absolute_range(view, data_offset, entry.compressed_size, [&](const uint8_t* data, size_t size) {
                size_t offset = 0;
                while (offset < size) {
                    check_cancelled(cancel);
                    if (header.size() < header_length) {
                        size_t take = std::min(header_length - header.size(), size - offset);
                        header.insert(header.end(), data + offset, data + offset + take);
                        offset += take;
                        if (header.size() == header_length) {
                            initialize_aes();
                        }
                        continue;
                    }
                    if (encrypted_seen < encrypted_payload_size) {
                        size_t take = static_cast<size_t>(std::min<uint64_t>(size - offset, encrypted_payload_size - encrypted_seen));
                        decrypt_aes_payload(data + offset, take);
                        encrypted_seen += take;
                        offset += take;
                        continue;
                    }
                    size_t take = std::min(auth_code_length - auth_code.size(), size - offset);
                    auth_code.insert(auth_code.end(), data + offset, data + offset + take);
                    offset += take;
                    if (take == 0) {
                        throw AppError("Unexpected trailing AES ZIP data for entry: " + entry.name);
                    }
                }
            }, cancel);

            if (header.size() != header_length || encrypted_seen != encrypted_payload_size || auth_code.size() != auth_code_length) {
                throw AppError("WinZip AES record is truncated for entry: " + entry.name);
            }
            std::vector<uint8_t> digest = hmac->finish();
            if (!std::equal(digest.begin(), digest.begin() + auth_code_length, auth_code.begin())) {
                throw AppError("WinZip AES authentication failed for entry: " + entry.name);
            }
        } else if ((entry.flags & 1) != 0) {
            if (password.empty()) {
                throw AppError("ZIP entry is encrypted but package password is empty: " + entry.name);
            }
            ZipCrypto crypto(wide_to_utf8(password));
            std::vector<uint8_t> header;
            header.reserve(12);
            std::vector<uint8_t> decrypted;
            decrypted.reserve(256 * 1024);
            stream_zip_absolute_range(view, data_offset, entry.compressed_size, [&](const uint8_t* data, size_t size) {
                decrypted.clear();
                for (size_t i = 0; i < size; ++i) {
                    uint8_t plain = crypto.decrypt(data[i]);
                    if (header.size() < 12) {
                        header.push_back(plain);
                    } else {
                        decrypted.push_back(plain);
                    }
                }
                if (!decrypted.empty()) {
                    feed_compressed(decrypted.data(), decrypted.size());
                }
            }, cancel);
            if (header.size() != 12) {
                throw AppError("ZIP encrypted payload is shorter than encryption header: " + entry.name);
            }
            if ((entry.flags & 0x0008) == 0) {
                uint8_t expected = static_cast<uint8_t>((entry.crc32 >> 24) & 0xFF);
                if (header[11] != expected) {
                    throw AppError("ZIP password check failed for entry: " + entry.name);
                }
            }
        } else {
            stream_zip_absolute_range(view, data_offset, entry.compressed_size, feed_compressed, cancel);
        }

        finish_method();
        cleanup_streams();
    } catch (...) {
        cleanup_streams();
        throw;
    }

    output.close();
    if (!output) {
        throw AppError("Could not write extracted file: " + printable_path(target));
    }
    if (plain_written != entry.uncompressed_size) {
        throw AppError("ZIP entry size mismatch: " + entry.name);
    }
    uint32_t final_crc = crc ^ 0xFFFFFFFFu;
    if (zip_entry_requires_crc(entry) && final_crc != entry.crc32) {
        std::ostringstream message;
        message << "ZIP CRC mismatch for " << entry.name << ": expected 0x"
                << std::hex << entry.crc32 << ", got 0x" << final_crc;
        throw AppError(message.str());
    }
}

void ensure_free_space_for_path(const fs::path& path, uint64_t required_bytes) {
    if (required_bytes == 0) {
        return;
    }
    fs::path query_path = path;
    std::error_code ec;
    while (!query_path.empty() && !fs::exists(query_path, ec)) {
        if (ec) {
            throw AppError("Could not inspect extraction path: " + ec.message());
        }
        fs::path parent = query_path.parent_path();
        if (parent == query_path) {
            break;
        }
        query_path = std::move(parent);
    }
    if (query_path.empty()) {
        throw AppError("Could not locate an existing volume for extraction path");
    }
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(query_path.wstring().c_str(), &available, nullptr, nullptr)) {
        throw AppError("Could not query free space for " + wide_to_utf8(query_path.wstring()) +
            ": " + format_win32_error(GetLastError()));
    }
    if (available.QuadPart < required_bytes) {
        throw AppError("Not enough free space for extraction: required=" + std::to_string(required_bytes) +
            ", available=" + std::to_string(available.QuadPart));
    }
}

ZipExtractResult extract_zip_stored_entries(const fs::path& zip_path,
    const fs::path& destination,
    const std::wstring& password,
    const CancellationCallback& cancel,
    uint64_t extraction_quota,
    std::optional<size_t> expected_file_count) {
    std::vector<ZipEntryInfo> entries = read_zip_central_directory(zip_path);
    uint64_t declared_total = 0;
    size_t declared_files = 0;
    for (const ZipEntryInfo& entry : entries) {
        if (entry.uncompressed_size > extraction_quota - declared_total) {
            throw AppError("ZIP declared uncompressed size exceeds extraction quota");
        }
        declared_total += entry.uncompressed_size;
        if (!zip_entry_is_directory(entry)) {
            ++declared_files;
        }
    }
    if (expected_file_count && declared_files != *expected_file_count) {
        throw AppError("ZIP file count does not match VerifyData: expected " +
            std::to_string(*expected_file_count) + ", actual " + std::to_string(declared_files));
    }

    struct PlannedEntry {
        size_t index = 0;
        fs::path relative;
        bool directory = false;
    };
    struct OutputNode {
        bool directory = false;
        bool explicit_entry = false;
    };
    std::vector<PlannedEntry> plan;
    plan.reserve(entries.size());
    std::map<std::wstring, OutputNode> output_nodes;
    for (size_t index = 0; index < entries.size(); ++index) {
        const ZipEntryInfo& entry = entries[index];
        check_cancelled(cancel);
        fs::path relative = safe_zip_relative_path(entry.name);
        bool directory = zip_entry_is_directory(entry);
        if (!directory && entry.method != 0 && entry.method != 8 && entry.method != 12 && entry.method != 99) {
            throw AppError("Unsupported ZIP method " + std::to_string(entry.method) +
                " (" + zip_method_name(entry.method) + ") for an entry");
        }

        fs::path ancestor;
        std::vector<fs::path> components;
        for (const fs::path& component : relative) {
            components.push_back(component);
        }
        for (size_t component_index = 0; component_index < components.size(); ++component_index) {
            ancestor /= components[component_index];
            std::wstring key = lower_w(ancestor.wstring());
            bool final_component = component_index + 1 == components.size();
            if (!final_component) {
                auto [it, inserted] = output_nodes.emplace(key, OutputNode{true, false});
                if (!inserted && !it->second.directory) {
                    throw AppError("ZIP output has a file/directory ancestor conflict");
                }
                continue;
            }
            auto [it, inserted] = output_nodes.emplace(key, OutputNode{directory, true});
            if (!inserted) {
                if (it->second.explicit_entry) {
                    throw AppError("ZIP contains duplicate case-insensitive output paths");
                }
                if (!directory || !it->second.directory) {
                    throw AppError("ZIP output has a file/directory conflict");
                }
                it->second.explicit_entry = true;
            }
        }
        plan.push_back(PlannedEntry{index, std::move(relative), directory});
    }

    reject_existing_reparse_points(destination, "ZIP extraction destination");
    ensure_free_space_for_path(destination, declared_total);
    fs::create_directories(destination);
    reject_existing_reparse_points(destination, "ZIP extraction destination");

    ZipExtractResult result;
    for (const PlannedEntry& planned : plan) {
        check_cancelled(cancel);
        const ZipEntryInfo& entry = entries[planned.index];
        fs::path target = ensure_path_within_root(destination,
            destination / planned.relative,
            "ZIP output path");
        if (planned.directory) {
            fs::create_directories(target);
            reject_existing_reparse_points(target, "ZIP output directory");
            ++result.directories;
            continue;
        }
        fs::create_directories(target.parent_path());
        reject_existing_reparse_points(target.parent_path(), "ZIP output directory");
        (void)ensure_path_within_root(destination, target, "ZIP output path");
        extract_zip_entry_to_file_streaming(zip_path, entry, target, password, cancel);
        check_cancelled(cancel);
        ++result.files;
    }
    return result;
}

std::vector<Brand> parse_brands_xml(const fs::path& path) {
    reject_existing_reparse_points(path, "brands.xml input");
    if (!fs::exists(path)) {
        throw AppError("brands.xml not found: " + printable_path(path));
    }
    if (!fs::is_regular_file(path)) {
        throw AppError("brands.xml is not an ordinary regular file: " + printable_path(path));
    }

    IStream* stream = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(path.wstring().c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &stream);
    if (FAILED(hr)) {
        throw AppError("SHCreateStreamOnFileEx failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }

    IXmlReader* reader = nullptr;
    hr = CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(&reader), nullptr);
    if (FAILED(hr)) {
        stream->Release();
        throw AppError("CreateXmlReader failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }
    reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    hr = reader->SetInput(stream);
    stream->Release();
    if (FAILED(hr)) {
        reader->Release();
        throw AppError("IXmlReader::SetInput failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }

    std::vector<Brand> brands;
    std::optional<Brand> current;
    std::wstring current_element;

    XmlNodeType node_type = XmlNodeType_None;
    while (S_OK == (hr = reader->Read(&node_type))) {
        const wchar_t* local_name = nullptr;
        UINT local_name_len = 0;
        switch (node_type) {
        case XmlNodeType_Element:
            reader->GetLocalName(&local_name, &local_name_len);
            current_element.assign(local_name ? local_name : L"", local_name_len);
            if (current_element == L"Brand") {
                current = Brand{};
            }
            break;
        case XmlNodeType_Text:
        case XmlNodeType_CDATA: {
            if (!current) {
                break;
            }
            const wchar_t* value = nullptr;
            UINT value_len = 0;
            reader->GetValue(&value, &value_len);
            std::wstring text(value ? value : L"", value_len);
            if (current_element == L"Id") current->id = text;
            else if (current_element == L"Name") current->name = text;
            else if (current_element == L"Repository") current->repository = text;
            else if (current_element == L"AliasName") current->alias_name = text;
            break;
        }
        case XmlNodeType_EndElement:
            reader->GetLocalName(&local_name, &local_name_len);
            if (std::wstring(local_name ? local_name : L"", local_name_len) == L"Brand" && current) {
                if (!current->id.empty()) {
                    validate_safe_path_component(current->id, "Brand.Id");
                }
                if (!current->alias_name.empty()) {
                    validate_safe_path_component(current->alias_name, "Brand.AliasName");
                }
                brands.push_back(*current);
                current.reset();
            }
            current_element.clear();
            break;
        default:
            break;
        }
    }

    reader->Release();
    if (FAILED(hr)) {
        throw AppError("IXmlReader::Read failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }
    return brands;
}

bool parse_xml_bool(const std::wstring& value, const std::string& context) {
    std::wstring normalized = lower_w(trim_w(value));
    if (normalized == L"true" || normalized == L"1") {
        return true;
    }
    if (normalized == L"false" || normalized == L"0" || normalized.empty()) {
        return false;
    }
    throw AppError("Invalid boolean value in " + context + ": " + wide_to_utf8(value));
}

uint64_t parse_uint64_decimal_strict(const std::wstring& value, const std::string& context) {
    std::wstring text = trim_w(value);
    if (text.empty()) {
        throw AppError(context + " is empty");
    }
    uint64_t result = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') {
            throw AppError(context + " is not an unsigned decimal integer: " + wide_to_utf8(value));
        }
        unsigned digit = static_cast<unsigned>(c - L'0');
        if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            throw AppError(context + " is out of range");
        }
        result = result * 10 + digit;
    }
    return result;
}

bool is_md5_hex_string(const std::wstring& value) {
    if (value.size() != 32) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
    });
}

struct ParsedIso8601DateTime {
    SYSTEMTIME value{};
    bool has_explicit_offset = false;
    int offset_minutes = 0;
};

bool parse_fixed_decimal(const std::wstring& text, size_t offset, size_t count, int& value) {
    if (offset + count > text.size()) {
        return false;
    }
    int parsed = 0;
    for (size_t i = 0; i < count; ++i) {
        wchar_t c = text[offset + i];
        if (c < L'0' || c > L'9') {
            return false;
        }
        parsed = parsed * 10 + (c - L'0');
    }
    value = parsed;
    return true;
}

bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool parse_iso8601_datetime(const std::wstring& value, ParsedIso8601DateTime& out) {
    out = {};
    std::wstring text = trim_w(value);
    if (text.size() < 10 || text[4] != L'-' || text[7] != L'-') {
        return false;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    if (!parse_fixed_decimal(text, 0, 4, year) ||
        !parse_fixed_decimal(text, 5, 2, month) ||
        !parse_fixed_decimal(text, 8, 2, day)) {
        return false;
    }

    int hour = 0;
    int minute = 0;
    int second = 0;
    int milliseconds = 0;
    size_t pos = 10;
    if (pos < text.size()) {
        if (text[pos] != L'T' && text[pos] != L't' && text[pos] != L' ') {
            return false;
        }
        ++pos;
        if (!parse_fixed_decimal(text, pos, 2, hour) || pos + 2 >= text.size() || text[pos + 2] != L':' ||
            !parse_fixed_decimal(text, pos + 3, 2, minute)) {
            return false;
        }
        pos += 5;
        if (pos < text.size() && text[pos] == L':') {
            ++pos;
            if (!parse_fixed_decimal(text, pos, 2, second)) {
                return false;
            }
            pos += 2;
        }
        if (pos < text.size() && (text[pos] == L'.' || text[pos] == L',')) {
            ++pos;
            size_t digits = 0;
            while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') {
                if (digits < 3) {
                    milliseconds = milliseconds * 10 + (text[pos] - L'0');
                }
                ++digits;
                ++pos;
            }
            if (digits == 0) {
                return false;
            }
            while (digits < 3) {
                milliseconds *= 10;
                ++digits;
            }
        }
        if (pos < text.size() && (text[pos] == L'Z' || text[pos] == L'z')) {
            out.has_explicit_offset = true;
            out.offset_minutes = 0;
            ++pos;
        } else if (pos < text.size() && (text[pos] == L'+' || text[pos] == L'-')) {
            int sign = text[pos] == L'+' ? 1 : -1;
            ++pos;
            int offset_hour = 0;
            int offset_minute = 0;
            if (!parse_fixed_decimal(text, pos, 2, offset_hour)) {
                return false;
            }
            pos += 2;
            if (pos < text.size() && text[pos] == L':') {
                ++pos;
            }
            if (!parse_fixed_decimal(text, pos, 2, offset_minute)) {
                return false;
            }
            pos += 2;
            if (offset_hour > 14 || offset_minute > 59 || (offset_hour == 14 && offset_minute != 0)) {
                return false;
            }
            out.has_explicit_offset = true;
            out.offset_minutes = sign * (offset_hour * 60 + offset_minute);
        }
    }
    if (pos != text.size() || year < 1 || year > 9999 || month < 1 || month > 12 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }
    static constexpr int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = days_in_month[month - 1] + (month == 2 && is_leap_year(year) ? 1 : 0);
    if (day < 1 || day > max_day) {
        return false;
    }

    out.value = {};
    out.value.wYear = static_cast<WORD>(year);
    out.value.wMonth = static_cast<WORD>(month);
    out.value.wDay = static_cast<WORD>(day);
    out.value.wHour = static_cast<WORD>(hour);
    out.value.wMinute = static_cast<WORD>(minute);
    out.value.wSecond = static_cast<WORD>(second);
    out.value.wMilliseconds = static_cast<WORD>(milliseconds);
    return true;
}

uint64_t iso8601_utc_ticks(const ParsedIso8601DateTime& parsed) {
    FILETIME file_time{};
    if (!SystemTimeToFileTime(&parsed.value, &file_time)) {
        throw AppError("Could not normalize ISO-8601 date: " + format_win32_error(GetLastError()));
    }
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    int offset_magnitude = parsed.offset_minutes < 0 ? -parsed.offset_minutes : parsed.offset_minutes;
    uint64_t offset_ticks = static_cast<uint64_t>(offset_magnitude) * 60ull * 10000000ull;
    if (parsed.offset_minutes >= 0) {
        if (ticks.QuadPart < offset_ticks) {
            throw AppError("ISO-8601 offset underflows FILETIME range");
        }
        ticks.QuadPart -= offset_ticks;
    } else {
        if (ticks.QuadPart > std::numeric_limits<uint64_t>::max() - offset_ticks) {
            throw AppError("ISO-8601 offset overflows FILETIME range");
        }
        ticks.QuadPart += offset_ticks;
    }
    return ticks.QuadPart;
}

bool start_date_is_future(const PartInfo& part) {
    if (trim_w(part.start_date).empty()) {
        return false;
    }

    ParsedIso8601DateTime parsed;
    if (!parse_iso8601_datetime(part.start_date, parsed)) {
        throw AppError("Invalid StartDate for part " + wide_to_utf8(part.part_number) +
            ": " + wide_to_utf8(part.start_date));
    }
    if (parsed.value.wYear < 1970) {
        return false;
    }
    if (parsed.has_explicit_offset) {
        FILETIME now_file_time{};
        GetSystemTimeAsFileTime(&now_file_time);
        ULARGE_INTEGER now_ticks{};
        now_ticks.LowPart = now_file_time.dwLowDateTime;
        now_ticks.HighPart = now_file_time.dwHighDateTime;
        return iso8601_utc_ticks(parsed) > now_ticks.QuadPart;
    }

    std::tm start_tm{};
    start_tm.tm_year = parsed.value.wYear - 1900;
    start_tm.tm_mon = parsed.value.wMonth - 1;
    start_tm.tm_mday = parsed.value.wDay;
    start_tm.tm_hour = parsed.value.wHour;
    start_tm.tm_min = parsed.value.wMinute;
    start_tm.tm_sec = parsed.value.wSecond;
    start_tm.tm_isdst = -1;
    std::time_t start = std::mktime(&start_tm);
    if (start == static_cast<std::time_t>(-1)) {
        throw AppError("Could not normalize StartDate for part " + wide_to_utf8(part.part_number));
    }
    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return start > now;
}

BrandData parse_brand_data_xml(const fs::path& path) {
    reject_existing_reparse_points(path, "brand data.xml input");
    if (!fs::exists(path)) {
        throw AppError("brand data.xml not found: " + printable_path(path));
    }
    if (!fs::is_regular_file(path)) {
        throw AppError("brand data.xml is not an ordinary regular file: " + printable_path(path));
    }

    IStream* stream = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(path.wstring().c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &stream);
    if (FAILED(hr)) {
        throw AppError("SHCreateStreamOnFileEx failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }

    IXmlReader* reader = nullptr;
    hr = CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(&reader), nullptr);
    if (FAILED(hr)) {
        stream->Release();
        throw AppError("CreateXmlReader failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }
    reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    hr = reader->SetInput(stream);
    stream->Release();
    if (FAILED(hr)) {
        reader->Release();
        throw AppError("IXmlReader::SetInput failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }

    BrandData data;
    std::optional<PartInfo> current_part;
    std::optional<ModelInfo> current_model;
    std::wstring current_element;

    XmlNodeType node_type = XmlNodeType_None;
    while (S_OK == (hr = reader->Read(&node_type))) {
        const wchar_t* local_name = nullptr;
        UINT local_name_len = 0;
        switch (node_type) {
        case XmlNodeType_Element:
            reader->GetLocalName(&local_name, &local_name_len);
            current_element.assign(local_name ? local_name : L"", local_name_len);
            if (current_element == L"Part") {
                current_part = PartInfo{};
            } else if (current_element == L"Model") {
                current_model = ModelInfo{};
            }
            break;
        case XmlNodeType_Text:
        case XmlNodeType_CDATA: {
            if (!current_part && !current_model) {
                break;
            }
            const wchar_t* value = nullptr;
            UINT value_len = 0;
            reader->GetValue(&value, &value_len);
            std::wstring text = trim_w(std::wstring(value ? value : L"", value_len));
            if (current_part) {
                if (current_element == L"PartNumber") current_part->part_number = text;
                else if (current_element == L"Remarks") current_part->remarks = text;
                else if (current_element == L"ReplacingPartNumber") current_part->replacing_part_number = text;
                else if (current_element == L"IsVisible") current_part->is_visible = parse_xml_bool(text, "Part.IsVisible");
                else if (current_element == L"StartDate") current_part->start_date = text;
                else if (current_element == L"ModelRef" && !text.empty()) current_part->model_refs.push_back(text);
            } else if (current_model) {
                if (current_element == L"Id") current_model->id = text;
                else if (current_element == L"Name") current_model->name = text;
                else if (current_element == L"Remarks") current_model->remarks = text;
            }
            break;
        }
        case XmlNodeType_EndElement:
            reader->GetLocalName(&local_name, &local_name_len);
            if (std::wstring(local_name ? local_name : L"", local_name_len) == L"Part" && current_part) {
                if (!current_part->part_number.empty() &&
                    is_safe_path_component(current_part->part_number) &&
                    (current_part->replacing_part_number.empty() ||
                        is_safe_path_component(current_part->replacing_part_number))) {
                    data.parts.push_back(*current_part);
                }
                current_part.reset();
            } else if (std::wstring(local_name ? local_name : L"", local_name_len) == L"Model" && current_model) {
                if (!current_model->id.empty() || !current_model->name.empty()) {
                    data.models.push_back(*current_model);
                }
                current_model.reset();
            }
            current_element.clear();
            break;
        default:
            break;
        }
    }

    reader->Release();
    if (FAILED(hr)) {
        throw AppError("IXmlReader::Read failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }
    std::sort(data.models.begin(), data.models.end(), [](const ModelInfo& left, const ModelInfo& right) {
        return left.name < right.name;
    });
    return data;
}

std::vector<PartInfo> parse_brand_parts_xml(const fs::path& path) {
    return parse_brand_data_xml(path).parts;
}

std::vector<ModelInfo> brand_models_with_all(const BrandData& data) {
    std::vector<ModelInfo> models;
    models.push_back(ModelInfo{L"0", L"All models", L""});
    models.insert(models.end(), data.models.begin(), data.models.end());
    return models;
}

fs::path brands_xml_path(const Config& cfg) {
    return cfg.metadata_dir / L"brands.xml";
}

fs::path brand_metadata_dir(const Config& cfg, const Brand& brand) {
    if (!trim_w(brand.id).empty()) {
        return safe_child_path(cfg.metadata_dir, brand.id, "Brand.Id");
    }
    validate_safe_path_component(brand.alias_name, "Brand.AliasName");
    fs::path brands_root = ensure_path_within_root(cfg.metadata_dir, cfg.metadata_dir / L"brands", "brands metadata directory");
    return safe_child_path(brands_root, brand.alias_name, "Brand.AliasName");
}

fs::path brand_data_path(const Config& cfg, const Brand& brand) {
    fs::path brand_dir = brand_metadata_dir(cfg, brand);
    return ensure_path_within_root(cfg.metadata_dir, brand_dir / L"data.xml", "brand data path");
}

fs::path package_xml_path(const Config& cfg, const Brand& brand, const std::wstring& part_number) {
    validate_safe_path_component(part_number, "Part.PartNumber");
    fs::path brand_dir = brand_metadata_dir(cfg, brand);
    return ensure_path_within_root(cfg.metadata_dir, brand_dir / (part_number + L".xml"), "package XML path");
}

fs::path ensure_brands_xml(const Config& cfg,
    bool refresh,
    const CancellationCallback& cancel) {
    fs::path target = brands_xml_path(cfg);
    if (refresh || !file_exists_nonempty(target)) {
        remove_empty_file(target);
        WinHttpClient client(cfg, cancel);
        (void)client.get_to_file(L"common/Settings/brands.xml", target, kMaxMetadataDownloadBytes);
    }
    return target;
}

Brand find_brand_or_throw(const Config& cfg, const std::wstring& brand_token, bool refresh) {
    fs::path path = ensure_brands_xml(cfg, refresh);
    std::vector<Brand> brands = parse_brands_xml(path);
    std::wstring token = trim_w(brand_token);
    for (const Brand& brand : brands) {
        if (iequals_w(brand.id, token) ||
            iequals_w(brand.alias_name, token) ||
            iequals_w(brand.name, token) ||
            (!brand.repository.empty() && iequals_w(brand.repository, token))) {
            return brand;
        }
    }
    throw AppError("Brand not found in brands.xml: " + wide_to_utf8(token));
}

fs::path ensure_brand_data_xml(const Config& cfg,
    const Brand& brand,
    bool refresh,
    const CancellationCallback& cancel) {
    fs::path target = brand_data_path(cfg, brand);
    validate_safe_path_component(brand.alias_name, "Brand.AliasName");
    fs::path legacy_brand_dir = ensure_path_within_root(cfg.metadata_dir,
        cfg.metadata_dir / L"brands" / brand.alias_name,
        "legacy brand metadata path");
    fs::path legacy_target = ensure_path_within_root(cfg.metadata_dir,
        legacy_brand_dir / L"data.xml",
        "legacy brand data path");
    if (!refresh && file_exists_nonempty(target)) {
        return target;
    }
    if (!refresh && file_exists_nonempty(legacy_target)) {
        return legacy_target;
    }
    remove_empty_file(target);
    remove_empty_file(legacy_target);

    if (brand.alias_name.empty()) {
        throw AppError("Brand has empty AliasName: " + wide_to_utf8(brand.name));
    }
    validate_safe_path_component(brand.alias_name, "Brand.AliasName");
    WinHttpClient client(cfg, cancel);
    (void)client.get_to_file(brand.alias_name + L"/data.xml", target, kMaxMetadataDownloadBytes);
    return target;
}

bool part_matches_model(const PartInfo& part, const std::wstring& model_id) {
    std::wstring id = trim_w(model_id);
    if (id.empty() || id == L"0") {
        return true;
    }
    return std::any_of(part.model_refs.begin(), part.model_refs.end(), [&](const std::wstring& model_ref) {
        return iequals_w(model_ref, id);
    });
}

const PartInfo& find_part_or_throw(const std::vector<PartInfo>& parts,
    const std::wstring& part_number,
    const std::wstring& model_id) {
    const PartInfo* match = nullptr;
    for (const PartInfo& part : parts) {
        if (!iequals_w(part.part_number, part_number) || !part_matches_model(part, model_id)) {
            continue;
        }
        if (match) {
            throw AppError("Multiple Part entries found in brand data.xml for part " + wide_to_utf8(part_number) +
                " and model " + wide_to_utf8(trim_w(model_id)));
        }
        match = &part;
    }
    if (!match) {
        throw AppError("Part not found in brand data.xml: " + wide_to_utf8(part_number) +
            " (model " + wide_to_utf8(trim_w(model_id).empty() ? L"0" : trim_w(model_id)) + ")");
    }
    return *match;
}

PartResolution resolve_part_by_original_rules(const std::vector<PartInfo>& parts,
    const std::wstring& requested_part_number,
    const std::wstring& model_id,
    bool include_hidden) {
    PartResolution resolution;
    resolution.requested_part_number = trim_w(requested_part_number);
    resolution.model_id = trim_w(model_id).empty() ? L"0" : trim_w(model_id);
    std::wstring current_number = resolution.requested_part_number;

    for (size_t depth = 0; depth < 64; ++depth) {
        for (const std::wstring& seen : resolution.chain) {
            if (iequals_w(seen, current_number)) {
                throw AppError("ReplacingPartNumber loop detected at part: " + wide_to_utf8(current_number));
            }
        }
        resolution.chain.push_back(current_number);

        const PartInfo& part = find_part_or_throw(parts, current_number, resolution.model_id);
        if (include_hidden) {
            resolution.resolved_part = part;
            return resolution;
        }

        std::wstring replacement = trim_w(part.replacing_part_number);
        if (!replacement.empty()) {
            current_number = replacement;
            continue;
        }
        if (!part.is_visible) {
            throw AppError("Part is not visible and has no replacement: " + wide_to_utf8(part.part_number));
        }
        if (start_date_is_future(part)) {
            throw AppError("Part StartDate is in the future: " + wide_to_utf8(part.part_number) +
                " (" + wide_to_utf8(part.start_date) + ")");
        }
        resolution.resolved_part = part;
        return resolution;
    }

    throw AppError("ReplacingPartNumber chain is too deep for part: " + wide_to_utf8(requested_part_number));
}

PartResolution resolve_part_by_original_rules(const std::vector<PartInfo>& parts,
    const std::wstring& requested_part_number,
    bool include_hidden) {
    return resolve_part_by_original_rules(parts, requested_part_number, L"0", include_hidden);
}

PartResolution resolve_brand_part(const Config& cfg,
    const Brand& brand,
    const std::wstring& part_number,
    const std::wstring& model_id,
    bool refresh,
    bool include_hidden,
    const CancellationCallback& cancel) {
    fs::path data_xml = ensure_brand_data_xml(cfg, brand, refresh, cancel);
    check_cancelled(cancel);
    std::vector<PartInfo> parts = parse_brand_parts_xml(data_xml);
    if (parts.empty()) {
        throw AppError("No Part entries found in brand data.xml: " + printable_path(data_xml));
    }
    return resolve_part_by_original_rules(parts, part_number, model_id, include_hidden);
}

fs::path ensure_package_xml(const Config& cfg,
    const Brand& brand,
    const std::wstring& part_number,
    bool refresh,
    const CancellationCallback& cancel) {
    fs::path target = package_xml_path(cfg, brand, part_number);
    if (refresh || !file_exists_nonempty(target)) {
        remove_empty_file(target);
        if (brand.alias_name.empty()) {
            throw AppError("Brand has empty AliasName: " + wide_to_utf8(brand.name));
        }
        validate_safe_path_component(brand.alias_name, "Brand.AliasName");
        WinHttpClient client(cfg, cancel);
        (void)client.get_to_file(
            brand.alias_name + L"/" + part_number + L".xml",
            target,
            kMaxMetadataDownloadBytes);
    }
    return target;
}

std::optional<std::wstring> xml_attribute(IXmlReader* reader, const std::wstring& wanted_name) {
    HRESULT hr = reader->MoveToFirstAttribute();
    if (hr == S_FALSE) {
        return std::nullopt;
    }
    if (FAILED(hr)) {
        throw AppError("IXmlReader::MoveToFirstAttribute failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }

    do {
        const wchar_t* local_name = nullptr;
        UINT local_name_len = 0;
        reader->GetLocalName(&local_name, &local_name_len);
        std::wstring name(local_name ? local_name : L"", local_name_len);
        if (name == wanted_name) {
            const wchar_t* value = nullptr;
            UINT value_len = 0;
            reader->GetValue(&value, &value_len);
            reader->MoveToElement();
            return std::wstring(value ? value : L"", value_len);
        }
        hr = reader->MoveToNextAttribute();
    } while (hr == S_OK);

    reader->MoveToElement();
    if (FAILED(hr)) {
        throw AppError("IXmlReader::MoveToNextAttribute failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }
    return std::nullopt;
}

PackageInfo parse_package_xml(const fs::path& path) {
    reject_existing_reparse_points(path, "package XML input");
    if (!fs::exists(path)) {
        throw AppError("package XML not found: " + printable_path(path));
    }
    if (!fs::is_regular_file(path)) {
        throw AppError("package XML is not an ordinary regular file: " + printable_path(path));
    }

    IStream* stream = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(path.wstring().c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &stream);
    if (FAILED(hr)) {
        throw AppError("SHCreateStreamOnFileEx failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }

    IXmlReader* reader = nullptr;
    hr = CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(&reader), nullptr);
    if (FAILED(hr)) {
        stream->Release();
        throw AppError("CreateXmlReader failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }
    reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    hr = reader->SetInput(stream);
    stream->Release();
    if (FAILED(hr)) {
        reader->Release();
        throw AppError("IXmlReader::SetInput failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }

    struct XmlReaderReleaseGuard {
        IXmlReader* value = nullptr;
        ~XmlReaderReleaseGuard() {
            if (value) {
                value->Release();
            }
        }
    } reader_guard{reader};

    PackageInfo package;
    std::wstring current_element;
    bool in_package = false;
    bool in_verify_data = false;
    bool in_checksums = false;
    bool checksums_present = false;
    std::vector<std::wstring> verify_path;
    std::vector<bool> verify_item_is_folder;

    XmlNodeType node_type = XmlNodeType_None;
    while (S_OK == (hr = reader->Read(&node_type))) {
        const wchar_t* local_name = nullptr;
        UINT local_name_len = 0;
        switch (node_type) {
        case XmlNodeType_Element:
            reader->GetLocalName(&local_name, &local_name_len);
            current_element.assign(local_name ? local_name : L"", local_name_len);
            if (current_element == L"Package") {
                in_package = true;
            } else if (current_element == L"Checksums") {
                if (!in_package || checksums_present || in_verify_data) {
                    throw AppError("Package XML has an invalid or duplicate Checksums element");
                }
                checksums_present = true;
                in_checksums = true;
            } else if (current_element == L"VerifyData") {
                if (!in_package || in_checksums || package.verify_data_present || in_verify_data) {
                    throw AppError("Package XML has an invalid or duplicate VerifyData element");
                }
                package.verify_data_present = true;
                in_verify_data = true;
                verify_path.clear();
                verify_item_is_folder.clear();
            } else if (current_element == L"Checksum") {
                if (!in_package || !in_checksums || in_verify_data) {
                    throw AppError("Checksum element is outside Package.Checksums");
                }
                ChecksumEntry entry;
                auto name = xml_attribute(reader, L"Name");
                auto value = xml_attribute(reader, L"Value");
                if (!name) {
                    throw AppError("Package Checksum is missing Name");
                }
                entry.filename = *name;
                validate_safe_path_component(entry.filename, "Package Checksum.Name");
                if (!value) {
                    throw AppError("Package Checksum is missing Value");
                }
                std::wstring checksum_value = *value;
                if (!is_md5_hex_string(checksum_value)) {
                    throw AppError("Package Checksum.Value must contain exactly 32 hexadecimal characters");
                }
                entry.md5 = wide_to_utf8(lower_w(std::move(checksum_value)));
                for (const ChecksumEntry& existing : package.checksums) {
                    if (iequals_w(existing.filename, entry.filename)) {
                        throw AppError("Package checksum filenames are not unique: " + wide_to_utf8(entry.filename));
                    }
                }
                package.checksums.push_back(std::move(entry));
            } else if (in_verify_data && current_element == L"Item") {
                if (!verify_item_is_folder.empty() && !verify_item_is_folder.back()) {
                    throw AppError("VerifyData File item cannot contain child items");
                }
                auto type_attribute = xml_attribute(reader, L"Type");
                auto name_attribute = xml_attribute(reader, L"Name");
                auto size_attribute = xml_attribute(reader, L"Size");
                if (!type_attribute || !name_attribute) {
                    throw AppError("VerifyData Item requires Type and Name attributes");
                }
                std::wstring type = trim_w(*type_attribute);
                std::wstring name = *name_attribute;
                validate_safe_path_component(name, "VerifyData Item.Name");
                bool is_folder = iequals_w(type, L"Folder");
                bool is_file = iequals_w(type, L"File");
                if (!is_folder && !is_file) {
                    throw AppError("VerifyData Item.Type must be File or Folder");
                }
                verify_item_is_folder.push_back(is_folder);
                if (is_folder) {
                    if (!size_attribute) {
                        throw AppError("VerifyData Folder item is missing Size");
                    }
                    if (!trim_w(*size_attribute).empty()) {
                        throw AppError("VerifyData Folder Size must be empty");
                    }
                    verify_path.push_back(name);
                } else if (is_file) {
                    if (!size_attribute) {
                        throw AppError("VerifyData File item is missing Size");
                    }
                    fs::path relative;
                    for (const std::wstring& segment : verify_path) {
                        relative /= segment;
                    }
                    relative /= name;
                    VerifyItemInfo item;
                    item.name = relative.wstring();
                    item.size = parse_uint64_decimal_strict(*size_attribute, "VerifyData File.Size");
                    for (const VerifyItemInfo& existing : package.verify_items) {
                        if (iequals_w(existing.name, item.name)) {
                            throw AppError("VerifyData contains a duplicate file path: " + wide_to_utf8(item.name));
                        }
                    }
                    package.verify_items.push_back(std::move(item));
                }
                if (reader->IsEmptyElement()) {
                    if (!verify_item_is_folder.empty()) {
                        if (verify_item_is_folder.back() && !verify_path.empty()) {
                            verify_path.pop_back();
                        }
                        verify_item_is_folder.pop_back();
                    }
                }
            }
            break;
        case XmlNodeType_Text:
        case XmlNodeType_CDATA: {
            if (!in_package || in_verify_data) {
                break;
            }
            const wchar_t* value = nullptr;
            UINT value_len = 0;
            reader->GetValue(&value, &value_len);
            std::wstring text = trim_w(std::wstring(value ? value : L"", value_len));
            if (current_element == L"Id") package.id = text;
            else if (current_element == L"Name") package.name = text;
            else if (current_element == L"Password") package.password = text;
            else if (current_element == L"Zugversion") package.zugversion = text;
            else if (current_element == L"SdFormat") package.sd_format = text;
            else if (current_element == L"SdType") package.sd_type = text;
            else if (current_element == L"Svm") package.svm = text;
            else if (current_element == L"Tpi") package.tpi = text;
            else if (current_element == L"Remarks") package.remarks = text;
            else if (current_element == L"LastUpdate") package.last_update = text;
            else if (current_element == L"Size") {
                try {
                    package.size_bytes = static_cast<uint64_t>(std::stoull(text));
                } catch (...) {
                    package.size_bytes = 0;
                }
            }
            else if (current_element == L"Capacity") {
                if (package.capacity_present) {
                    throw AppError("Package contains duplicate Capacity values");
                }
                uint64_t parsed = parse_uint64_decimal_strict(text, "Package Capacity");
                if (parsed > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                    throw AppError("Package Capacity is out of range");
                }
                package.recommended_capacity_gb = static_cast<int>(parsed);
                package.capacity_present = true;
            }
            else if (current_element == L"Count") {
                if (package.count_present) {
                    throw AppError("Package contains duplicate Count values");
                }
                uint64_t parsed = parse_uint64_decimal_strict(text, "Package Count");
                if (parsed > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                    throw AppError("Package Count is out of range");
                }
                package.count = static_cast<int>(parsed);
                package.count_present = true;
            }
            break;
        }
        case XmlNodeType_EndElement:
            reader->GetLocalName(&local_name, &local_name_len);
            if (std::wstring(local_name ? local_name : L"", local_name_len) == L"Package") {
                in_package = false;
            } else if (std::wstring(local_name ? local_name : L"", local_name_len) == L"Checksums") {
                in_checksums = false;
            } else if (std::wstring(local_name ? local_name : L"", local_name_len) == L"VerifyData") {
                in_verify_data = false;
                verify_path.clear();
                verify_item_is_folder.clear();
            } else if (in_verify_data && std::wstring(local_name ? local_name : L"", local_name_len) == L"Item") {
                if (!verify_item_is_folder.empty()) {
                    if (verify_item_is_folder.back() && !verify_path.empty()) {
                        verify_path.pop_back();
                    }
                    verify_item_is_folder.pop_back();
                }
            }
            current_element.clear();
            break;
        default:
            break;
        }
    }

    if (FAILED(hr)) {
        throw AppError("IXmlReader::Read failed: HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
    }
    validate_package_info(package, path);
    return package;
}

std::wstring verify_key(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return lower_w(value);
}

void insert_verify_file_size(std::map<std::wstring, uint64_t>& files,
    const std::wstring& key,
    uint64_t size) {
    if (!files.emplace(key, size).second) {
        throw AppError("VerifyData scan found duplicate case-insensitive file paths");
    }
}

std::map<std::wstring, uint64_t> collect_file_sizes_for_verify(
    const fs::path& root,
    const std::vector<fs::path>& excluded_files = {}) {
    reject_existing_reparse_points(root, "VerifyData root");
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) {
        throw AppError("VerifyData root is not an accessible ordinary directory: " + printable_path(root) +
            (ec ? ": " + ec.message() : ""));
    }
    std::map<std::wstring, uint64_t> files;
    std::map<std::wstring, bool> excluded_normalized;
    for (const fs::path& path : excluded_files) {
        excluded_normalized[lower_w(absolute_normalized_path(path))] = true;
    }
    const std::wstring normalized_root = lower_w(absolute_normalized_path(root));
    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec);
    if (ec) {
        throw AppError("Could not enumerate VerifyData root: " + ec.message());
    }
    fs::recursive_directory_iterator end;
    while (it != end) {
        const fs::path current = it->path();
        DWORD attributes = GetFileAttributesW(current.wstring().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            throw AppError("Could not inspect VerifyData object: " + printable_path(current) + ": " +
                format_win32_error(GetLastError()));
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw AppError("VerifyData scan rejects reparse points: " + printable_path(current));
        }

        ec.clear();
        fs::file_status status = it->symlink_status(ec);
        if (ec) {
            throw AppError("Could not read VerifyData object type: " + printable_path(current) + ": " + ec.message());
        }
        const bool is_directory = fs::is_directory(status);
        const bool is_regular = fs::is_regular_file(status);
        if (is_directory &&
            iequals_w(current.filename().wstring(), L"System Volume Information") &&
            lower_w(absolute_normalized_path(current.parent_path())) == normalized_root) {
            it.disable_recursion_pending();
        } else if (is_directory) {
            // Ordinary directories are structural and contain the files that are verified below.
        } else if (!is_regular) {
            throw AppError("VerifyData scan found a non-regular filesystem object: " + printable_path(current));
        } else if (excluded_normalized.find(lower_w(absolute_normalized_path(current))) == excluded_normalized.end()) {
            ec.clear();
            fs::path relative = fs::relative(current, root, ec);
            if (ec || relative.empty() || relative.has_root_path()) {
                throw AppError("Could not derive a safe VerifyData relative path for " + printable_path(current) +
                    (ec ? ": " + ec.message() : ""));
            }
            ec.clear();
            uintmax_t size = fs::file_size(current, ec);
            if (ec || size > (std::numeric_limits<uint64_t>::max)()) {
                throw AppError("Could not read VerifyData file size for " + printable_path(current) +
                    (ec ? ": " + ec.message() : ""));
            }
            insert_verify_file_size(files, verify_key(relative.wstring()), static_cast<uint64_t>(size));
        }

        ec.clear();
        it.increment(ec);
        if (ec) {
            throw AppError("VerifyData directory enumeration failed: " + ec.message());
        }
    }
    return files;
}

void verify_package_output(const fs::path& destination,
    const PackageInfo& package,
    const std::vector<fs::path>& excluded_files,
    VerifyFileSetPolicy file_set_policy) {
    if (package.verify_items.empty()) {
        throw AppError("Package has no VerifyData entries");
    }
    std::map<std::wstring, uint64_t> actual = collect_file_sizes_for_verify(destination, excluded_files);
    if (file_set_policy == VerifyFileSetPolicy::Exact && actual.size() != package.verify_items.size()) {
        throw AppError("VerifyData file count mismatch: expected " + std::to_string(package.verify_items.size()) +
            ", actual " + std::to_string(actual.size()));
    }
    for (const VerifyItemInfo& item : package.verify_items) {
        std::wstring key = verify_key(item.name);
        auto it = actual.find(key);
        if (it == actual.end()) {
            throw AppError("VerifyData missing file: " + wide_to_utf8(item.name));
        }
        if (it->second != item.size) {
            throw AppError("VerifyData size mismatch for " + wide_to_utf8(item.name) +
                ": expected " + std::to_string(item.size) + ", actual " + std::to_string(it->second));
        }
    }
}

std::string md5_hex(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw AppError("Could not open file for MD5: " + printable_path(path));
    }

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    require_win32(CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT), "CryptAcquireContext");
    auto cleanup = [&]() {
        if (hash) CryptDestroyHash(hash);
        if (provider) CryptReleaseContext(provider, 0);
    };
    if (!CryptCreateHash(provider, CALG_MD5, 0, 0, &hash)) {
        cleanup();
        throw AppError("CryptCreateHash(MD5) failed: " + format_win32_error(GetLastError()));
    }

    std::vector<char> buffer(64 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize got = input.gcount();
        if (got > 0) {
            if (!CryptHashData(hash, reinterpret_cast<BYTE*>(buffer.data()), static_cast<DWORD>(got), 0)) {
                cleanup();
                throw AppError("CryptHashData failed: " + format_win32_error(GetLastError()));
            }
        }
    }

    BYTE digest[16]{};
    DWORD digest_size = sizeof(digest);
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_size, 0)) {
        cleanup();
        throw AppError("CryptGetHashParam failed: " + format_win32_error(GetLastError()));
    }
    cleanup();

    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(32);
    for (BYTE b : digest) {
        output.push_back(hex[(b >> 4) & 0xF]);
        output.push_back(hex[b & 0xF]);
    }
    return output;
}



void validate_package_info(const PackageInfo& package, const fs::path& package_xml) {
    if (!package.count_present || package.count <= 0) {
        throw AppError("Package Count is missing or not positive: " + printable_path(package_xml));
    }
    if (!package.capacity_present || package.recommended_capacity_gb <= 0) {
        throw AppError("Package Capacity is missing or not positive: " + printable_path(package_xml));
    }
    if (package.checksums.empty()) {
        throw AppError("Package has no checksum entries: " + printable_path(package_xml));
    }
    if (static_cast<size_t>(package.count) != package.checksums.size()) {
        throw AppError("Package count mismatch: Count=" + std::to_string(package.count) +
            ", checksum entries=" + std::to_string(package.checksums.size()));
    }
    if (!package.verify_data_present || package.verify_items.empty()) {
        throw AppError("Package has no valid VerifyData file entries: " + printable_path(package_xml));
    }
    for (size_t i = 0; i < package.checksums.size(); ++i) {
        const ChecksumEntry& entry = package.checksums[i];
        validate_safe_path_component(entry.filename, "Package Checksum.Name");
        if (entry.md5.size() != 32 || !std::all_of(entry.md5.begin(), entry.md5.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            })) {
            throw AppError("Package Checksum.Value must contain exactly 32 hexadecimal characters");
        }
        for (size_t j = 0; j < i; ++j) {
            if (iequals_w(package.checksums[j].filename, entry.filename)) {
                throw AppError("Package checksum filenames are not unique: " + wide_to_utf8(entry.filename));
            }
        }
    }
}

uint64_t package_extraction_quota(const PackageInfo& package) {
    uint64_t total = 0;
    for (const VerifyItemInfo& item : package.verify_items) {
        if (item.size > std::numeric_limits<uint64_t>::max() - total) {
            throw AppError("VerifyData total size is out of range");
        }
        total += item.size;
    }
    return total;
}

struct PayloadDownloadItem {
    size_t package_index = 0;
    ChecksumEntry checksum;
    fs::path target;
    std::wstring remote;
    size_t attempts = 0;
};

struct PayloadDownloadAttempt {
    PayloadDownloadItem item;
    bool success = false;
    std::string error;
};

void validate_downloaded_payload(const PayloadDownloadItem& item) {
    if (!item.checksum.md5.empty()) {
        std::string got = md5_hex(item.target);
        if (got != item.checksum.md5) {
            throw AppError("MD5 mismatch for " + printable_path(item.target) +
                ": expected " + item.checksum.md5 + ", got " + got);
        }
    }
}

void validate_and_commit_payload_candidate(const fs::path& candidate, const PayloadDownloadItem& item) {
    try {
        PayloadDownloadItem downloaded = item;
        downloaded.target = candidate;
        validate_downloaded_payload(downloaded);
        replace_with_downloaded_file(candidate, item.target);
    } catch (...) {
        std::error_code cleanup_error;
        fs::remove(candidate, cleanup_error);
        throw;
    }
}

PayloadDownloadAttempt download_payload_attempt(const Config& cfg,
    PayloadDownloadItem item,
    const CancellationCallback& cancel = {}) {
    PayloadDownloadAttempt result;
    result.item = std::move(item);
    fs::path partial_target;
    try {
        partial_target = download_partial_path(result.item.target);
        check_cancelled(cancel);
        WinHttpClient client(cfg, cancel);
        (void)client.get_to_file_resumable(result.item.remote, partial_target);
        check_cancelled(cancel);
        validate_and_commit_payload_candidate(partial_target, result.item);
        result.success = true;
    } catch (const std::exception& ex) {
        result.error = ex.what();
        if (is_cancelled_message(result.error)) {
            std::error_code cleanup_error;
            fs::remove(partial_target, cleanup_error);
        }
    } catch (...) {
        std::error_code cleanup_error;
        fs::remove(partial_target, cleanup_error);
        result.error = "Unknown payload download error";
    }
    return result;
}

std::vector<fs::path> download_payload_files(const Config& cfg,
    const std::wstring& brand_alias,
    const PackageInfo& package,
    const PayloadProgressCallback& progress,
    const PayloadConcurrencyCallback& concurrency,
    const CancellationCallback& cancel) {
    validate_safe_path_component(brand_alias, "Brand.AliasName");
    fs::create_directories(cfg.temp_dir);
    const size_t total = package.checksums.size();
    std::vector<fs::path> payload_files(total);
    std::vector<PayloadDownloadItem> pending;
    pending.reserve(total);
    std::vector<fs::path> downloaded_this_run;
    size_t completed = 0;

    auto cleanup_downloads = [&]() {
        std::error_code ec;
        for (const fs::path& path : downloaded_this_run) {
            fs::remove(path, ec);
            ec.clear();
        }
    };

    auto check_cancelled_with_cleanup = [&]() {
        try {
            check_cancelled(cancel);
        } catch (...) {
            cleanup_downloads();
            throw;
        }
    };

    const size_t max_workers = 4;
    const size_t max_attempts = 4;
    size_t worker_limit = 1;
    bool saw_error = false;
    bool multithreaded = false;

    for (const ChecksumEntry& entry : package.checksums) {
        check_cancelled_with_cleanup();
        size_t index = pending.size() + completed + 1;
        validate_safe_path_component(entry.filename, "Package Checksum.Name");
        fs::path local_name(entry.filename);
        fs::path target = safe_child_path(cfg.temp_dir, entry.filename, "Package Checksum.Name");
        payload_files[index - 1] = target;
        bool target_exists = fs::exists(target);
        bool existing_valid = false;
        if (target_exists) {
            existing_valid = entry.md5.empty() || md5_hex(target) == entry.md5;
        }
        bool needs_download = !existing_valid;
        if (needs_download) {
            PayloadDownloadItem item;
            item.package_index = index;
            item.checksum = entry;
            item.target = target;
            item.remote = brand_alias + L"/" + local_name.wstring();
            pending.push_back(std::move(item));
        } else {
            validate_downloaded_payload(PayloadDownloadItem{index, entry, target, L"", 0});
            ++completed;
            if (progress) {
                progress(completed, total, target, false, 0.0);
            }
        }
    }

    uint64_t downloaded_bytes = 0;
    auto transfer_started = std::chrono::steady_clock::now();
    while (!pending.empty()) {
        check_cancelled_with_cleanup();
        size_t batch_size = std::min(worker_limit, pending.size());
        std::vector<PayloadDownloadItem> batch;
        batch.reserve(batch_size);
        batch.insert(batch.end(), pending.begin(), pending.begin() + batch_size);
        pending.erase(pending.begin(), pending.begin() + batch_size);

        std::vector<PayloadDownloadAttempt> results(batch_size);
        std::vector<std::jthread> workers;
        workers.reserve(batch_size);
        for (size_t i = 0; i < batch_size; ++i) {
            workers.emplace_back([&, i]() {
                PayloadDownloadItem item = batch[i];
                ++item.attempts;
                results[i] = download_payload_attempt(cfg, std::move(item), cancel);
            });
        }
        for (std::jthread& worker : workers) {
            worker.join();
        }
        check_cancelled_with_cleanup();

        bool round_failed = false;
        std::string fatal_error;
        for (PayloadDownloadAttempt& result : results) {
            if (result.success) {
                payload_files[result.item.package_index - 1] = result.item.target;
                downloaded_this_run.push_back(result.item.target);
                ++completed;
                downloaded_bytes += file_size_or_zero(result.item.target);
                if (progress) {
                    double elapsed = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - transfer_started).count();
                    double network_mbps = elapsed > 0.0
                        ? static_cast<double>(downloaded_bytes) / 1024.0 / 1024.0 / elapsed
                        : 0.0;
                    progress(completed, total, result.item.target, true, network_mbps);
                }
            } else {
                round_failed = true;
                saw_error = true;
                bool terminal_failure = false;
                if (result.item.attempts >= max_attempts) {
                    fatal_error = result.error;
                    terminal_failure = true;
                } else if (is_cancelled_message(result.error)) {
                    fatal_error = result.error;
                    terminal_failure = true;
                } else {
                    pending.push_back(std::move(result.item));
                }
            }
        }

        if (!fatal_error.empty()) {
            if (is_cancelled_message(fatal_error)) {
                cleanup_downloads();
            }
            throw AppError(fatal_error);
        }

        if (round_failed) {
            if (worker_limit > 1) {
                --worker_limit;
                if (worker_limit == 1 && multithreaded) {
                    multithreaded = false;
                    if (concurrency) concurrency(false, worker_limit);
                }
            }
        } else if (!saw_error && worker_limit < max_workers && !pending.empty()) {
            ++worker_limit;
            if (worker_limit == 2 && !multithreaded) {
                multithreaded = true;
                if (concurrency) concurrency(true, worker_limit);
            }
        }
    }

    if (multithreaded && concurrency) {
        concurrency(false, 1);
    }

    return payload_files;
}

fs::path select_zip_payload(const std::vector<fs::path>& payload_files) {
    std::optional<fs::path> selected;
    std::optional<fs::path> numbered_base;
    std::map<unsigned int, fs::path> numbered_parts;
    for (const fs::path& path : payload_files) {
        std::wstring extension = path.extension().wstring();
        if (iequals_w(extension, L".zip")) {
            if (selected) {
                throw AppError("Package checksum list contains more than one final .zip payload");
            }
            selected = path;
            continue;
        }

        if (extension.size() != 4 || extension[0] != L'.' ||
            !iswdigit(extension[1]) || !iswdigit(extension[2]) || !iswdigit(extension[3]) ||
            !iequals_w(path.stem().extension().wstring(), L".zip")) {
            continue;
        }

        unsigned int part_number = static_cast<unsigned int>(std::stoul(extension.substr(1)));
        if (part_number == 0) {
            throw AppError("Numbered ZIP payload parts must start at .001");
        }
        fs::path base = path.parent_path() / path.stem();
        if (numbered_base && !iequals_w(numbered_base->wstring(), base.wstring())) {
            throw AppError("Package checksum list contains multiple numbered ZIP payload families");
        }
        numbered_base = base;
        if (!numbered_parts.emplace(part_number, path).second) {
            throw AppError("Package checksum list contains duplicate numbered ZIP payload parts");
        }
    }
    if (selected && numbered_base) {
        throw AppError("Package checksum list mixes a final .zip payload with numbered .zip.NNN parts");
    }
    if (selected) {
        return *selected;
    }
    if (!numbered_parts.empty()) {
        unsigned int expected = 1;
        for (const auto& [part_number, path] : numbered_parts) {
            (void)path;
            if (part_number != expected) {
                throw AppError("Numbered ZIP payload sequence has a missing part");
            }
            ++expected;
        }
        return numbered_parts.rbegin()->second;
    }
    throw AppError("No ZIP payload found in package checksum list");
}

fs::path unique_staging_dir(const Config& cfg) {
    reject_existing_reparse_points(cfg.temp_dir, "Staging root");
    fs::create_directories(cfg.temp_dir);
    reject_existing_reparse_points(cfg.temp_dir, "Staging root");
    for (int attempt = 0; attempt < 100; ++attempt) {
        fs::path path = ensure_path_within_root(cfg.temp_dir, cfg.temp_dir /
            (L"stage-" +
                std::to_wstring(GetCurrentProcessId()) +
                L"-" +
                std::to_wstring(GetCurrentThreadId()) +
                L"-" +
                std::to_wstring(GetTickCount64()) +
                L"-" + std::to_wstring(attempt)),
            "Staging directory");
        std::error_code ec;
        if (fs::create_directory(path, ec)) {
            reject_existing_reparse_points(path, "Staging directory");
            return path;
        }
        if (ec && !fs::exists(path)) {
            throw AppError("Could not create staging directory: " + printable_path(path) + ": " + ec.message());
        }
    }
    throw AppError("Could not create a unique staging directory under " + printable_path(cfg.temp_dir));
}

void copy_staged_tree_to_destination(const fs::path& staging,
    const fs::path& destination,
    const TextProgressCallback& progress,
    const CancellationCallback& cancel,
    const std::function<void(const std::vector<fs::path>&)>& validate_before_commit) {
    (void)progress;
    struct CopiedFile {
        fs::path target;
        fs::path backup;
        bool installed = false;
    };

    reject_existing_reparse_points(staging, "Staging source");
    std::error_code staging_status_error;
    if (!fs::is_directory(staging, staging_status_error) || staging_status_error) {
        throw AppError("Staging source is not an accessible ordinary directory: " + printable_path(staging) +
            (staging_status_error ? ": " + staging_status_error.message() : ""));
    }
    reject_existing_reparse_points(destination, "Copy destination");
    const std::wstring normalized_destination = absolute_normalized_path(destination);
    auto checked_destination_path = [&](const fs::path& path, const std::string& context) {
        std::wstring normalized = absolute_normalized_path(path);
        if (iequals_w(normalized, normalized_destination)) {
            reject_existing_reparse_points(normalized_destination, normalized_destination, context);
            return fs::path(normalized);
        }
        return ensure_path_within_root(fs::path(normalized_destination), fs::path(normalized), context);
    };

    std::vector<CopiedFile> copied_files;
    std::vector<fs::path> temp_files;
    std::vector<fs::path> created_directories;
    auto unique_sibling = [](const fs::path& target, const wchar_t* suffix) {
        std::wstring name = target.filename().wstring() +
            suffix +
            std::to_wstring(GetCurrentProcessId()) +
            L"." +
            std::to_wstring(GetCurrentThreadId()) +
            L"." +
            std::to_wstring(GetTickCount64());
        return target.parent_path() / name;
    };
    auto cleanup = [&]() {
        std::string failures;
        auto record_failure = [&](const char* operation, const fs::path& path, const std::error_code& error) {
            if (!failures.empty()) {
                failures += "; ";
            }
            failures += operation;
            failures += " ";
            failures += printable_path(path);
            failures += ": ";
            failures += error.message();
        };
        std::error_code ec;
        for (const fs::path& temp : temp_files) {
            fs::remove(temp, ec);
            if (ec) {
                record_failure("could not remove temporary file", temp, ec);
            }
            ec.clear();
        }
        for (auto it = copied_files.rbegin(); it != copied_files.rend(); ++it) {
            if (it->installed) {
                fs::remove(it->target, ec);
                if (ec) {
                    record_failure("could not remove installed file", it->target, ec);
                }
                ec.clear();
            }
            if (!it->backup.empty()) {
                bool backup_exists = fs::exists(it->backup, ec);
                if (ec) {
                    record_failure("could not inspect backup file", it->backup, ec);
                } else if (backup_exists) {
                    fs::rename(it->backup, it->target, ec);
                    if (ec) {
                        record_failure("could not restore backup file", it->backup, ec);
                    }
                }
                ec.clear();
            }
        }
        for (auto it = created_directories.rbegin(); it != created_directories.rend(); ++it) {
            fs::remove(*it, ec);
            if (ec) {
                record_failure("could not remove created directory", *it, ec);
            }
            ec.clear();
        }
        return failures;
    };
    auto create_directories_tracked = [&](const fs::path& path) {
        fs::path checked_path = checked_destination_path(path, "Copy destination directory");
        std::vector<fs::path> missing;
        fs::path current = checked_path;
        std::error_code ec;
        while (!current.empty() && !fs::exists(current, ec)) {
            if (ec) {
                throw AppError("Could not inspect destination directory: " + printable_path(current) + ": " + ec.message());
            }
            missing.push_back(current);
            fs::path parent = current.parent_path();
            if (parent == current) {
                break;
            }
            current = std::move(parent);
        }
        for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
            created_directories.push_back(*it);
        }
        fs::create_directories(checked_path);
        (void)checked_destination_path(checked_path, "Copy destination directory");
    };
    try {
        create_directories_tracked(destination);
        std::error_code iteration_error;
        fs::recursive_directory_iterator iterator(staging, fs::directory_options::none, iteration_error);
        if (iteration_error) {
            throw AppError("Could not enumerate staging tree: " + iteration_error.message());
        }
        fs::recursive_directory_iterator end;
        while (iterator != end) {
            check_cancelled(cancel);
            const fs::directory_entry& entry = *iterator;
            DWORD attributes = GetFileAttributesW(entry.path().wstring().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                throw AppError("Could not inspect staging object: " + printable_path(entry.path()) + ": " +
                    format_win32_error(GetLastError()));
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                throw AppError("Staging tree contains a reparse point: " + printable_path(entry.path()));
            }
            std::error_code type_error;
            fs::file_status status = entry.symlink_status(type_error);
            if (type_error) {
                throw AppError("Could not inspect staging object type: " + type_error.message());
            }
            std::error_code relative_error;
            fs::path relative = fs::relative(entry.path(), staging, relative_error);
            if (relative_error || relative.empty() || relative.has_root_path()) {
                throw AppError("Could not derive a safe staging relative path" +
                    (relative_error ? ": " + relative_error.message() : ""));
            }
            fs::path target = checked_destination_path(destination / relative, "Copy destination path");
            if (fs::is_directory(status)) {
                create_directories_tracked(target);
            } else if (fs::is_regular_file(status)) {
                create_directories_tracked(target.parent_path());
                (void)checked_destination_path(target, "Copy destination file");
                if (fs::exists(target) && !fs::is_regular_file(target)) {
                    throw AppError("Destination path is not a file: " + printable_path(target));
                }
                fs::path temp = unique_sibling(target, L".mmidownloader-copy.");
                temp = checked_destination_path(temp, "Copy temporary file");
                temp_files.push_back(temp);
                fs::copy_file(entry.path(), temp, fs::copy_options::overwrite_existing);
                CopiedFile copied;
                copied.target = target;
                copied_files.push_back(copied);
                CopiedFile& active = copied_files.back();
                if (fs::exists(target)) {
                    active.backup = unique_sibling(target, L".mmidownloader-backup.");
                    active.backup = checked_destination_path(active.backup, "Copy backup file");
                    fs::rename(target, active.backup);
                }
                fs::rename(temp, target);
                active.installed = true;
                temp_files.pop_back();
                check_cancelled(cancel);
            } else {
                throw AppError("Staging tree contains a non-regular filesystem object: " + printable_path(entry.path()));
            }
            iteration_error.clear();
            iterator.increment(iteration_error);
            if (iteration_error) {
                throw AppError("Staging directory enumeration failed: " + iteration_error.message());
            }
        }
        if (validate_before_commit) {
            std::vector<fs::path> backup_files;
            backup_files.reserve(copied_files.size());
            for (const CopiedFile& copied : copied_files) {
                if (!copied.backup.empty()) {
                    backup_files.push_back(copied.backup);
                }
            }
            validate_before_commit(backup_files);
        }
    } catch (...) {
        std::exception_ptr original_error = std::current_exception();
        std::string rollback_failures = cleanup();
        if (!rollback_failures.empty()) {
            throw AppError("Package copy failed and rollback was incomplete; preserved backup paths are listed where applicable: " +
                rollback_failures);
        }
        std::rethrow_exception(original_error);
    }

    std::error_code ec;
    for (const CopiedFile& copied : copied_files) {
        if (!copied.backup.empty()) {
            fs::remove(copied.backup, ec);
            if (ec) {
                throw AppError("Package was installed, but its backup file could not be removed: " +
                    printable_path(copied.backup) + ": " + ec.message());
            }
            ec.clear();
        }
    }
}

ZipExtractResult extract_package_to_destination_with_staging(const Config& cfg,
    const fs::path& zip_payload,
    const fs::path& destination,
    const PackageInfo& package,
    const std::wstring& password,
    const TextProgressCallback& progress,
    const CancellationCallback& cancel) {
    fs::path staging = unique_staging_dir(cfg);
    std::error_code cleanup_ec;
    try {
        uint64_t extraction_quota = package_extraction_quota(package);
        check_cancelled(cancel);
        if (progress) {
            progress(L"Staging: extract ZIP to temp");
        }
        ZipExtractResult extract = extract_zip_stored_entries(
            zip_payload,
            staging,
            password,
            cancel,
            extraction_quota,
            package.verify_items.size());

        check_cancelled(cancel);
        if (progress) {
            progress(L"Staging: verify extracted package");
        }
        verify_package_output(staging, package);

        check_cancelled(cancel);
        if (progress) {
            progress(L"Copy: staged package to destination");
        }
        ensure_free_space_for_path(destination, extraction_quota);
        copy_staged_tree_to_destination(staging,
            destination,
            progress,
            cancel,
            [&](const std::vector<fs::path>& backup_files) {
                check_cancelled(cancel);
                if (progress) {
                    progress(L"Validation: VerifyData");
                }
                verify_package_output(destination,
                    package,
                    backup_files,
                    VerifyFileSetPolicy::AllowAdditionalFiles);
                check_cancelled(cancel);
            });

        fs::remove_all(staging, cleanup_ec);
        return extract;
    } catch (...) {
        fs::remove_all(staging, cleanup_ec);
        throw;
    }
}

} // namespace mmid
