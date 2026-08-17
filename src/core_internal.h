#pragma once

#include "core.h"

#include <bcrypt.h>

#include <filesystem>
#include <string>

namespace mmid {

void require_ntstatus(NTSTATUS status, const char* operation);
void require_win32(bool ok, const char* operation);
std::wstring lower_w(std::wstring value);
std::wstring absolute_normalized_path(const std::filesystem::path& path);
void validate_safe_path_component(
    const std::wstring& value,
    const std::string& context);
void reject_existing_reparse_points(
    const std::filesystem::path& path,
    const std::string& context);
void reject_existing_reparse_points(
    const std::wstring& normalized_root,
    const std::wstring& normalized_candidate,
    const std::string& context);
std::filesystem::path ensure_path_within_root(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate,
    const std::string& context);
std::filesystem::path safe_child_path(
    const std::filesystem::path& root,
    const std::wstring& component,
    const std::string& context);
uint64_t parse_uint64_decimal_strict(
    const std::wstring& value,
    const std::string& context);
bool is_md5_hex_string(const std::wstring& value);
std::filesystem::path download_partial_path(const std::filesystem::path& target);
uint64_t file_size_or_zero(const std::filesystem::path& path);
void replace_with_downloaded_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target);
DWORD download_catalog_file_resumable(
    const Config& config,
    const std::wstring& catalog_relative,
    const std::filesystem::path& target,
    const CancellationCallback& cancel);

struct BCryptAlgHandle final {
    BCRYPT_ALG_HANDLE value = nullptr;

    BCryptAlgHandle() = default;
    BCryptAlgHandle(const BCryptAlgHandle&) = delete;
    BCryptAlgHandle& operator=(const BCryptAlgHandle&) = delete;

    ~BCryptAlgHandle() {
        if (value) {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    }
};

struct BCryptKeyHandle final {
    BCRYPT_KEY_HANDLE value = nullptr;

    BCryptKeyHandle() = default;
    BCryptKeyHandle(const BCryptKeyHandle&) = delete;
    BCryptKeyHandle& operator=(const BCryptKeyHandle&) = delete;

    ~BCryptKeyHandle() {
        if (value) {
            BCryptDestroyKey(value);
        }
    }
};

} // namespace mmid
