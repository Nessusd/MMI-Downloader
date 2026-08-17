#include "core.h"
#include "core_internal.h"

#include <bzlib.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace mmid {
namespace {

constexpr size_t kMaxZipCentralEntries = 65534;
constexpr size_t kMaxZipExplicitDirectories = 65534;
constexpr uint64_t kMaxZipCentralDirectoryBytes = 256ull * 1024ull * 1024ull;

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
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            values[i] = c;
        }
        return values;
    }();
    return table[(crc ^ value) & 0xFF] ^ (crc >> 8);
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

} // namespace

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


} // namespace mmid
