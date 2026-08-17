#include "core.h"
#include "core_internal.h"

#include <wincrypt.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <xmllite.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace mmid {
namespace {

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

} // namespace

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

namespace {

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

} // namespace

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

namespace {

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

} // namespace

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

namespace {

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
        (void)download_catalog_file_resumable(
            cfg, result.item.remote, partial_target, cancel);
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

} // namespace

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
