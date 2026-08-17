#include "package_preparation.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace mmid {
namespace {

constexpr double kMinimumReadSpeedMbps = 6.0;

struct PreparedPackage {
    PartResolution resolution;
    fs::path metadata_file;
    PackageInfo package;
    std::wstring report_file;
};

void report_progress(
    const PreparationProgressCallback& progress,
    int value,
    const std::wstring& text) {
    if (progress) {
        progress(value, text);
    }
}

PreparedPackage resolve_package(
    const Config& config,
    const PreparationRequest& request,
    const PreparationProgressCallback& progress,
    const CancellationCallback& cancel) {
    report_progress(progress, 15, L"Preparing: searching part");
    PartResolution resolution = resolve_brand_part(
        config,
        request.brand,
        request.part_number,
        request.model_id,
        true,
        request.include_hidden,
        cancel);

    check_cancelled(cancel);
    report_progress(progress, 25, L"Downloading: package XML");
    fs::path metadata_file = ensure_package_xml(
        config,
        request.brand,
        resolution.resolved_part.part_number,
        true,
        cancel);
    check_cancelled(cancel);

    PackageInfo package = parse_package_xml(metadata_file);
    PreparedPackage result;
    result.resolution = std::move(resolution);
    result.metadata_file = std::move(metadata_file);
    result.package = std::move(package);
    if (!result.package.checksums.empty()) {
        result.report_file = fs::path(result.package.checksums.front().filename).filename().wstring();
    }
    return result;
}

std::vector<fs::path> download_payloads(
    const Config& config,
    const Brand& brand,
    const PackageInfo& package,
    int initial_progress,
    int progress_range,
    const PreparationProgressCallback& progress,
    const CancellationCallback& cancel) {
    return download_payload_files(
        config,
        brand.alias_name,
        package,
        [initial_progress, progress_range, &progress](
            size_t index,
            size_t total,
            const fs::path& path,
            bool downloaded,
            double network_mbps) {
            int value = initial_progress;
            if (total > 0) {
                value += static_cast<int>((index * progress_range) / total);
            }

            std::wstring text = std::wstring(downloaded ? L"Download: " : L"Cache: ") +
                path.filename().wstring() +
                L" [" + std::to_wstring(index) + L"/" + std::to_wstring(total) + L"]";
            if (downloaded && network_mbps > 0.0) {
                std::wostringstream speed;
                speed << std::fixed << std::setprecision(2) << network_mbps << L" MB/s";
                text += L" | " + speed.str();
            }
            report_progress(progress, value, text);
        },
        [initial_progress, &progress](bool enabled, size_t workers) {
            (void)workers;
            report_progress(progress,
                initial_progress,
                enabled ? L"Multithreaded download active" : L"Multithreaded download disabled");
        },
        cancel);
}

void download_extract_and_verify(
    const Config& config,
    const PreparationRequest& request,
    const PackageInfo& package,
    OperationStatus& failure_status,
    const PreparationProgressCallback& progress,
    const CancellationCallback& cancel) {
    check_cancelled(cancel);
    report_progress(progress, 45, L"Downloading: package files");
    failure_status = OperationStatus::BtacNoWebDavConnection;
    const std::vector<fs::path> payload_files = download_payloads(
        config, request.brand, package, 45, 20, progress, cancel);

    check_cancelled(cancel);
    failure_status = OperationStatus::PackageVerifyFailed;
    const fs::path zip_payload = select_zip_payload(payload_files);

    report_progress(progress, 70, L"Extracting: ZIP to temporary folder");
    failure_status = OperationStatus::PackageExtractFailed;
    const std::wstring password = decrypt_legacy_package_password(package.password);
    (void)extract_package_to_destination_with_staging(
        config,
        zip_payload,
        request.destination,
        package,
        password,
        [&progress, &failure_status](const std::wstring& text) {
            if (text.find(L"Validation:") == 0 || text.find(L"Staging: verify") == 0) {
                failure_status = OperationStatus::PackageVerifyFailed;
            } else if (text.find(L"Copy:") == 0) {
                failure_status = OperationStatus::PackageExtractFailed;
            }
            report_progress(progress, 80, text);
        },
        cancel);
}

fs::path download_and_stage(
    const Config& config,
    const PreparationRequest& request,
    const PackageInfo& package,
    OperationStatus& failure_status,
    const PreparationProgressCallback& progress,
    const CancellationCallback& cancel) {
    check_cancelled(cancel);
    report_progress(progress, 38, L"Downloading: package files");
    failure_status = OperationStatus::BtacNoWebDavConnection;
    const std::vector<fs::path> payload_files = download_payloads(
        config, request.brand, package, 38, 22, progress, cancel);

    check_cancelled(cancel);
    failure_status = OperationStatus::PackageVerifyFailed;
    const fs::path zip_payload = select_zip_payload(payload_files);
    fs::path staging = unique_staging_dir(config);
    std::error_code cleanup_error;
    try {
        report_progress(progress, 62, L"Staging: extracting ZIP before format");
        failure_status = OperationStatus::PackageExtractFailed;
        const std::wstring password = decrypt_legacy_package_password(package.password);
        (void)extract_zip_stored_entries(
            zip_payload,
            staging,
            password,
            cancel,
            package_extraction_quota(package),
            package.verify_items.size());

        check_cancelled(cancel);
        report_progress(progress, 75, L"Staging: VerifyData before format");
        failure_status = OperationStatus::PackageVerifyFailed;
        verify_package_output(staging, package);
        return staging;
    } catch (...) {
        fs::remove_all(staging, cleanup_error);
        throw;
    }
}

bool try_log_success(
    const Config& config,
    const std::wstring& brand,
    const std::wstring& part_number,
    const std::wstring& file_name,
    const std::chrono::system_clock::time_point& started) noexcept {
    try {
        return log_operation_report(
            config,
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

void log_failure(
    const Config& config,
    const Brand& brand,
    const std::wstring& report_part,
    const std::wstring& report_file,
    const std::chrono::system_clock::time_point& started,
    OperationStatus fallback_status,
    const std::exception& exception) {
    const OperationStatus status = classify_operation_failure(fallback_status, exception.what());
    try {
        log_error_entry(config, status, exception_message(exception));
        log_operation_report(
            config,
            brand.name,
            report_part,
            report_file,
            started,
            std::chrono::system_clock::now(),
            status);
    } catch (...) {
    }
}

} // namespace

PackagePreparationService::PackagePreparationService(
    Config config,
    RemovableDriveOperations& drive_operations)
    : config_(std::move(config)), drive_operations_(drive_operations) {}

PreparationOutcome PackagePreparationService::prepare(
    const PreparationRequest& request,
    const PreparationProgressCallback& progress,
    const CancellationCallback& cancel) const {
    switch (request.target) {
    case PreparationTarget::Folder:
        return prepare_to_folder(request, progress, cancel);
    case PreparationTarget::RemovableDrive:
        return prepare_to_removable_drive(request, progress, cancel);
    }
    throw AppError("Unsupported preparation target");
}

PreparationOutcome PackagePreparationService::prepare_to_folder(
    const PreparationRequest& request,
    const PreparationProgressCallback& progress,
    const CancellationCallback& cancel) const {
    const auto started = std::chrono::system_clock::now();
    OperationStatus failure_status = OperationStatus::MetaFileError;
    std::wstring report_part = trim_w(request.part_number);
    std::wstring report_file;

    try {
        check_cancelled(cancel);
        PreparedPackage prepared = resolve_package(config_, request, progress, cancel);
        check_cancelled(cancel);
        report_part = prepared.resolution.resolved_part.part_number;
        report_file = prepared.report_file;
        failure_status = OperationStatus::PackageVerifyFailed;
        validate_package_info(prepared.package, prepared.metadata_file);
        download_extract_and_verify(
            config_, request, prepared.package, failure_status, progress, cancel);
        check_cancelled(cancel);

        PreparationOutcome outcome;
        outcome.extracted_to_folder = true;
        outcome.success_log_failed = !try_log_success(
            config_, request.brand.name, report_part, report_file, started);
        return outcome;
    } catch (const std::exception& exception) {
        log_failure(
            config_, request.brand, report_part, report_file, started, failure_status, exception);
        throw;
    }
}

PreparationOutcome PackagePreparationService::prepare_to_removable_drive(
    const PreparationRequest& request,
    const PreparationProgressCallback& progress,
    const CancellationCallback& cancel) const {
    const auto started = std::chrono::system_clock::now();
    OperationStatus failure_status = OperationStatus::MetaFileError;
    std::wstring report_part = trim_w(request.part_number);
    std::wstring report_file;
    fs::path staging_to_cleanup;
    const std::wstring destination = request.destination.wstring();

    try {
        check_cancelled(cancel);
        PreparedPackage prepared = resolve_package(config_, request, progress, cancel);
        check_cancelled(cancel);
        report_part = prepared.resolution.resolved_part.part_number;
        report_file = prepared.report_file;
        failure_status = OperationStatus::PackageVerifyFailed;
        validate_package_info(prepared.package, prepared.metadata_file);

        failure_status = OperationStatus::NoRecommendedCapacity;
        if (!is_drive_root_path(destination)) {
            throw AppError("Drive workflow requires a drive root destination");
        }
        drive_operations_.validate_destination(destination, prepared.package);
        drive_operations_.ensure_temporary_directory_is_safe(config_.temp_dir, request.destination);

        report_progress(progress, 30, L"Preparing: checking read speed");
        check_cancelled(cancel);
        failure_status = OperationStatus::SpeedNotSuffient;
        const double read_speed = drive_operations_.measure_read_speed(destination, cancel);
        if (read_speed < kMinimumReadSpeedMbps) {
            throw AppError("SD card read speed is below required 6 MB/s: " +
                std::to_string(read_speed) + " MB/s");
        }

        staging_to_cleanup = download_and_stage(
            config_, request, prepared.package, failure_status, progress, cancel);

        report_progress(progress, 80, L"Preparing: revalidating removable drive");
        check_cancelled(cancel);
        failure_status = OperationStatus::NoRecommendedCapacity;
        drive_operations_.validate_destination(destination, prepared.package);

        report_progress(progress, 82, L"Preparing: formatting removable drive");
        check_cancelled(cancel);
        failure_status = OperationStatus::SdCardFormatFailed;
        drive_operations_.format(destination, prepared.package, cancel);
        check_cancelled(cancel);

        report_progress(progress, 86, L"Commit: copying verified staging to removable drive");
        failure_status = OperationStatus::PackageExtractFailed;
        ensure_free_space_for_path(request.destination, package_extraction_quota(prepared.package));
        copy_staged_tree_to_destination(
            staging_to_cleanup,
            request.destination,
            [&progress](const std::wstring& text) {
                report_progress(progress, 90, text);
            },
            cancel,
            [&](const std::vector<fs::path>& backup_files) {
                check_cancelled(cancel);
                report_progress(progress, 95, L"Validation: VerifyData on removable drive");
                failure_status = OperationStatus::PackageVerifyFailed;
                verify_package_output(request.destination, prepared.package, backup_files);
                check_cancelled(cancel);
            });

        std::error_code cleanup_error;
        fs::remove_all(staging_to_cleanup, cleanup_error);
        staging_to_cleanup.clear();

        report_progress(progress, 96, L"Finishing: renaming volume");
        check_cancelled(cancel);
        failure_status = OperationStatus::Exception;
        const std::optional<std::string> rename_error = drive_operations_.rename(
            destination, prepared.resolution.resolved_part.part_number);

        report_progress(progress, 98, L"Finishing: ejecting removable drive");
        failure_status = OperationStatus::CardEjectFailed;
        drive_operations_.eject(destination, cancel);

        if (rename_error) {
            failure_status = OperationStatus::Exception;
            throw AppError(
                "Package was verified and the drive was ejected, but the volume could not be renamed: " +
                *rename_error);
        }

        PreparationOutcome outcome;
        outcome.extracted_to_folder = false;
        outcome.success_log_failed = !try_log_success(
            config_, request.brand.name, report_part, report_file, started);
        return outcome;
    } catch (const std::exception& exception) {
        if (!staging_to_cleanup.empty()) {
            std::error_code cleanup_error;
            fs::remove_all(staging_to_cleanup, cleanup_error);
        }
        log_failure(
            config_, request.brand, report_part, report_file, started, failure_status, exception);
        throw;
    }
}

} // namespace mmid
