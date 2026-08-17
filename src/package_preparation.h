#pragma once

#include "core.h"
#include "removable_drive.h"

#include <filesystem>
#include <functional>
#include <string>

namespace mmid {

enum class PreparationTarget {
    Folder,
    RemovableDrive
};

struct PreparationRequest {
    Brand brand;
    std::wstring part_number;
    std::wstring model_id;
    std::filesystem::path destination;
    bool include_hidden = false;
    PreparationTarget target = PreparationTarget::Folder;
};

struct PreparationOutcome {
    bool extracted_to_folder = false;
    bool success_log_failed = false;
};

using PreparationProgressCallback = std::function<void(int value, const std::wstring& text)>;

class PackagePreparationService final {
public:
    PackagePreparationService(Config config, RemovableDriveOperations& drive_operations);

    PreparationOutcome prepare(
        const PreparationRequest& request,
        const PreparationProgressCallback& progress,
        const CancellationCallback& cancel = {}) const;

private:
    PreparationOutcome prepare_to_folder(
        const PreparationRequest& request,
        const PreparationProgressCallback& progress,
        const CancellationCallback& cancel) const;
    PreparationOutcome prepare_to_removable_drive(
        const PreparationRequest& request,
        const PreparationProgressCallback& progress,
        const CancellationCallback& cancel) const;

    Config config_;
    RemovableDriveOperations& drive_operations_;
};

} // namespace mmid
