#pragma once

#include "core.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mmid {

struct RemovableDriveInfo {
    std::wstring root;
    std::wstring label;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
};

std::wstring normalize_drive_root(std::wstring value);
bool is_drive_root_path(const std::wstring& value);
std::optional<RemovableDriveInfo> removable_drive_info(const std::wstring& root);
std::vector<RemovableDriveInfo> enumerate_removable_drives();

class RemovableDriveOperations {
public:
    virtual ~RemovableDriveOperations() = default;

    virtual void ensure_temporary_directory_is_safe(
        const std::filesystem::path& temporary_directory,
        const std::filesystem::path& destination) const = 0;
    virtual void validate_destination(
        const std::wstring& destination,
        const PackageInfo& package) const = 0;
    virtual double measure_read_speed(
        const std::wstring& root,
        const CancellationCallback& cancel) const = 0;
    virtual void format(
        const std::wstring& root,
        const PackageInfo& package,
        const CancellationCallback& cancel) const = 0;
    virtual std::optional<std::string> rename(
        const std::wstring& root,
        const std::wstring& part_number) const = 0;
    virtual void eject(
        const std::wstring& root,
        const CancellationCallback& cancel) const = 0;
};

class WindowsRemovableDriveOperations final : public RemovableDriveOperations {
public:
    void ensure_temporary_directory_is_safe(
        const std::filesystem::path& temporary_directory,
        const std::filesystem::path& destination) const override;
    void validate_destination(
        const std::wstring& destination,
        const PackageInfo& package) const override;
    double measure_read_speed(
        const std::wstring& root,
        const CancellationCallback& cancel) const override;
    void format(
        const std::wstring& root,
        const PackageInfo& package,
        const CancellationCallback& cancel) const override;
    std::optional<std::string> rename(
        const std::wstring& root,
        const std::wstring& part_number) const override;
    void eject(
        const std::wstring& root,
        const CancellationCallback& cancel) const override;
};

} // namespace mmid
