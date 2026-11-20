#pragma once

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mmid {

class AppError : public std::runtime_error {
public:
    explicit AppError(const std::string& message) : std::runtime_error(message) {}
};

using CancellationCallback = std::function<void()>;
using PayloadProgressCallback = std::function<void(
    std::size_t completed,
    std::size_t total,
    const std::filesystem::path& path,
    bool downloaded,
    double network_mbps)>;
using PayloadConcurrencyCallback = std::function<void(bool enabled, std::size_t workers)>;
using TextProgressCallback = std::function<void(const std::wstring&)>;

enum class SourceTransport {
    Auto,
    WebDav,
    File,
    Url,
    Ftp
};

enum class FtpMode {
    Auto,
    Passive,
    Active
};

enum class OperationStatus {
    Success,
    NoRecommendedCapacity,
    SpeedNotSuffient,
    SdCardFormatFailed,
    BtacNoWebDavConnection,
    LocalStorageNotEnough,
    PackageExtractFailed,
    PackageHashValidationFailed,
    PackageVerifyFailed,
    VerifyStructureNotFound,
    CardEjectFailed,
    CancelByUser,
    Exception,
    MetaFileError
};

enum class VerifyFileSetPolicy {
    Exact,
    AllowAdditionalFiles
};

struct Config {
    Config();

    std::wstring client_id;
    bool https = true;
    std::wstring server;
    std::wstring top_level_collection;
    std::wstring distribution_channel;
    SourceTransport source_transport = SourceTransport::Auto;
    std::wstring catalog_root;
    bool use_local_catalog = false;
    std::filesystem::path local_catalog_root;
    FtpMode ftp_mode = FtpMode::Auto;
    std::wstring username;
    std::string password_dpapi;
    std::filesystem::path metadata_dir;
    std::filesystem::path temp_dir;
    std::filesystem::path extract_dir;
    bool logging_enabled = false;
    std::filesystem::path log_dir;
};

struct Brand {
    std::wstring id;
    std::wstring name;
    std::wstring repository;
    std::wstring alias_name;
};

struct PartInfo {
    std::wstring part_number;
    std::wstring remarks;
    std::wstring replacing_part_number;
    bool is_visible = false;
    std::wstring start_date;
    std::vector<std::wstring> model_refs;
};

struct ModelInfo {
    std::wstring id;
    std::wstring name;
    std::wstring remarks;
};

struct BrandData {
    std::vector<PartInfo> parts;
    std::vector<ModelInfo> models;
};

struct PartResolution {
    std::wstring requested_part_number;
    std::wstring model_id;
    PartInfo resolved_part;
    std::vector<std::wstring> chain;
};

struct VerifyItemInfo {
    std::wstring name;
    uint64_t size = 0;
};

struct ChecksumEntry {
    std::wstring filename;
    std::string md5;
};

struct PackageInfo {
    std::wstring id;
    std::wstring name;
    int count = 0;
    uint64_t size_bytes = 0;
    int recommended_capacity_gb = 0;
    std::wstring zugversion;
    std::wstring sd_format;
    std::wstring sd_type;
    std::wstring svm;
    std::wstring tpi;
    std::wstring remarks;
    std::wstring password;
    std::wstring last_update;
    bool count_present = false;
    bool capacity_present = false;
    bool verify_data_present = false;
    std::vector<ChecksumEntry> checksums;
    std::vector<VerifyItemInfo> verify_items;
};

struct ZipExtractResult {
    std::size_t files = 0;
    std::size_t directories = 0;
    std::size_t skipped = 0;
};

const char* app_version() noexcept;
const char* build_number() noexcept;
std::wstring app_user_agent();

void check_cancelled(const CancellationCallback& cancel);
bool is_cancelled_message(const std::string& message);
std::string format_win32_error(DWORD error);
std::wstring utf8_to_wide(const std::string& input);
std::string wide_to_utf8(const std::wstring& input);
std::wstring trim_w(std::wstring value);
std::wstring trim_slashes(std::wstring value);
bool iequals_w(std::wstring_view left, std::wstring_view right);
std::filesystem::path executable_dir();
std::string printable_path(const std::filesystem::path& path);

std::string protect_password(const std::wstring& password);
std::wstring unprotect_password(const std::string& encoded);
std::wstring decrypt_legacy_package_password(const std::wstring& encrypted);

bool config_has_credentials(const Config& cfg);
void validate_config_credentials_complete(const Config& cfg);
std::wstring ftp_mode_to_string(FtpMode value);
bool looks_like_url(const std::wstring& value);
bool looks_like_ftp_url(const std::wstring& value);
// An absent file stays in memory only. Existing files missing client_id are migrated in place.
Config load_config();
void save_config(const Config& cfg);
std::wstring webdav_home_url(const Config& cfg);
std::wstring catalog_network_url_full(const Config& cfg, const std::wstring& relative);
bool legacy_webdav_source_is_configured(const Config& cfg);
bool catalog_source_is_configured(const Config& cfg);
bool credentials_target_an_https_source(const Config& cfg);
std::wstring catalog_location_description(const Config& cfg, const std::wstring& webdav_relative);
DWORD probe_catalog_source(
    const Config& cfg,
    const std::wstring& catalog_relative,
    const CancellationCallback& cancel = {});

std::filesystem::path log_dir(const Config& cfg);
bool log_error_entry(
    const Config& cfg,
    OperationStatus status,
    const std::wstring& message,
    const std::wstring& level = L"Error") noexcept;
bool log_operation_report(
    const Config& cfg,
    const std::wstring& brand,
    const std::wstring& part_number,
    const std::wstring& file_name,
    std::chrono::system_clock::time_point start,
    std::chrono::system_clock::time_point end,
    OperationStatus status) noexcept;
OperationStatus classify_operation_failure(OperationStatus fallback, const std::string& message);

std::vector<Brand> parse_brands_xml(const std::filesystem::path& path);
BrandData parse_brand_data_xml(const std::filesystem::path& path);
std::vector<ModelInfo> brand_models_with_all(const BrandData& data);
std::filesystem::path ensure_brands_xml(
    const Config& cfg,
    bool refresh,
    const CancellationCallback& cancel = {});
std::filesystem::path ensure_brand_data_xml(
    const Config& cfg,
    const Brand& brand,
    bool refresh,
    const CancellationCallback& cancel = {});
bool part_matches_model(const PartInfo& part, const std::wstring& model_id);
bool start_date_is_future(const PartInfo& part);
PartResolution resolve_brand_part(
    const Config& cfg,
    const Brand& brand,
    const std::wstring& part_number,
    const std::wstring& model_id,
    bool refresh,
    bool include_hidden,
    const CancellationCallback& cancel = {});
std::filesystem::path ensure_package_xml(
    const Config& cfg,
    const Brand& brand,
    const std::wstring& part_number,
    bool refresh,
    const CancellationCallback& cancel = {});
PackageInfo parse_package_xml(const std::filesystem::path& path);
void validate_package_info(const PackageInfo& package, const std::filesystem::path& package_xml);

std::vector<std::filesystem::path> download_payload_files(
    const Config& cfg,
    const std::wstring& brand_alias,
    const PackageInfo& package,
    const PayloadProgressCallback& progress = {},
    const PayloadConcurrencyCallback& concurrency = {},
    const CancellationCallback& cancel = {});
std::filesystem::path select_zip_payload(const std::vector<std::filesystem::path>& payload_files);
uint64_t package_extraction_quota(const PackageInfo& package);
void ensure_free_space_for_path(const std::filesystem::path& path, uint64_t required_bytes);
ZipExtractResult extract_zip_stored_entries(
    const std::filesystem::path& zip_path,
    const std::filesystem::path& destination,
    const std::wstring& password,
    const CancellationCallback& cancel = {},
    uint64_t extraction_quota = (std::numeric_limits<uint64_t>::max)(),
    std::optional<std::size_t> expected_file_count = std::nullopt);
std::filesystem::path unique_staging_dir(const Config& cfg);
void copy_staged_tree_to_destination(
    const std::filesystem::path& staging,
    const std::filesystem::path& destination,
    const TextProgressCallback& progress = {},
    const CancellationCallback& cancel = {},
    const std::function<void(const std::vector<std::filesystem::path>&)>& validate_before_commit = {});
ZipExtractResult extract_package_to_destination_with_staging(
    const Config& cfg,
    const std::filesystem::path& zip_payload,
    const std::filesystem::path& destination,
    const PackageInfo& package,
    const std::wstring& password,
    const TextProgressCallback& progress = {},
    const CancellationCallback& cancel = {});
void verify_package_output(
    const std::filesystem::path& destination,
    const PackageInfo& package,
    const std::vector<std::filesystem::path>& excluded_files = {},
    VerifyFileSetPolicy file_set_policy = VerifyFileSetPolicy::Exact);

} // namespace mmid
