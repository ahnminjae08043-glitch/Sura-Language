#ifndef SURA_UEFI_DISK_H
#define SURA_UEFI_DISK_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

struct SuraUefiDiskResult {
    std::vector<uint8_t> image;
    uint64_t partition_first_lba = 0;
    uint64_t partition_last_lba = 0;
    uint32_t fat_sectors = 0;
    uint32_t file_first_cluster = 0;
};

namespace SuraUefiDisk {

static constexpr uint32_t sector_size = 512;
static constexpr uint64_t partition_first_lba = 2048;
static constexpr uint32_t reserved_sectors = 32;
static constexpr uint32_t fat_count = 2;
static constexpr uint32_t sectors_per_cluster = 1;

inline void put16(std::vector<uint8_t>& out, size_t offset, uint16_t value) {
    out.at(offset) = static_cast<uint8_t>(value);
    out.at(offset + 1) = static_cast<uint8_t>(value >> 8);
}

inline void put32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        out.at(offset + i) = static_cast<uint8_t>(value >> (i * 8));
    }
}

inline void put64(std::vector<uint8_t>& out, size_t offset, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        out.at(offset + i) = static_cast<uint8_t>(value >> (i * 8));
    }
}

inline uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask =
                static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U));
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

inline uint64_t fnv1a64(const std::vector<uint8_t>& data) {
    uint64_t hash = 1469598103934665603ULL;
    for (uint8_t value : data) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

inline void write_short_entry(std::vector<uint8_t>& image, size_t offset,
                              const char (&name)[12], uint8_t attributes,
                              uint32_t cluster, uint32_t size) {
    std::memcpy(image.data() + offset, name, 11);
    image.at(offset + 11) = attributes;
    put16(image, offset + 20, static_cast<uint16_t>(cluster >> 16));
    put16(image, offset + 26, static_cast<uint16_t>(cluster));
    put32(image, offset + 28, size);
}

inline SuraUefiDiskResult build(const std::vector<uint8_t>& efi) {
    if (efi.size() < 1024 || efi[0] != 'M' || efi[1] != 'Z') {
        throw std::runtime_error(
            "UEFI disk image requires a valid PE32+ EFI payload");
    }
    if (efi.size() > (512ULL << 20)) {
        throw std::runtime_error("UEFI payload is too large for the disk builder");
    }

    const uint64_t file_clusters =
        std::max<uint64_t>(1, (efi.size() + sector_size - 1) / sector_size);
    const uint64_t desired_clusters =
        std::max<uint64_t>(65525, file_clusters + 3 + 2048);
    const uint64_t desired_fat =
        (desired_clusters + 2 + 127) / 128;
    const uint64_t desired_partition =
        reserved_sectors + fat_count * desired_fat + desired_clusters;
    uint64_t total_sectors = align_up(
        partition_first_lba + 33 + desired_partition, 2048);
    total_sectors = std::max<uint64_t>(total_sectors, 131072);
    if (total_sectors > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("UEFI disk image exceeds FAT32 size limits");
    }

    const uint64_t backup_header_lba = total_sectors - 1;
    const uint64_t backup_entries_lba = total_sectors - 33;
    const uint64_t partition_last_lba = total_sectors - 34;
    const uint64_t partition_sectors =
        partition_last_lba - partition_first_lba + 1;
    const uint64_t fat_sectors64 =
        (partition_sectors - reserved_sectors + 131) / 130;
    if (!fat_sectors64 ||
        fat_sectors64 > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("invalid FAT32 geometry");
    }
    const uint32_t fat_sectors = static_cast<uint32_t>(fat_sectors64);
    const uint64_t data_sectors =
        partition_sectors - reserved_sectors -
        static_cast<uint64_t>(fat_count) * fat_sectors;
    const uint64_t cluster_count = data_sectors / sectors_per_cluster;
    if (cluster_count < 65525 || cluster_count >= 0x0ffffff0ULL ||
        file_clusters + 3 > cluster_count) {
        throw std::runtime_error("UEFI payload does not fit the FAT32 partition");
    }
    if (fat_sectors64 * sector_size / 4 < cluster_count + 2) {
        throw std::runtime_error("FAT32 table is too small for its data area");
    }

    const uint64_t image_bytes64 = total_sectors * sector_size;
    if (image_bytes64 > std::numeric_limits<size_t>::max()) {
        throw std::runtime_error("UEFI disk image exceeds host address space");
    }
    SuraUefiDiskResult result;
    result.image.resize(static_cast<size_t>(image_bytes64), 0);
    result.partition_first_lba = partition_first_lba;
    result.partition_last_lba = partition_last_lba;
    result.fat_sectors = fat_sectors;
    result.file_first_cluster = 5;
    auto& image = result.image;

    // Protective MBR.
    const size_t mbr_partition = 446;
    image[mbr_partition + 1] = 0x00;
    image[mbr_partition + 2] = 0x02;
    image[mbr_partition + 3] = 0x00;
    image[mbr_partition + 4] = 0xee;
    image[mbr_partition + 5] = 0xff;
    image[mbr_partition + 6] = 0xff;
    image[mbr_partition + 7] = 0xff;
    put32(image, mbr_partition + 8, 1);
    put32(image, mbr_partition + 12,
          static_cast<uint32_t>(std::min<uint64_t>(
              total_sectors - 1, 0xffffffffULL)));
    image[510] = 0x55;
    image[511] = 0xaa;

    // One GPT EFI System Partition entry.
    std::vector<uint8_t> entries(128 * 128, 0);
    const uint8_t esp_type[16] = {
        0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
        0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
    };
    std::copy(std::begin(esp_type), std::end(esp_type), entries.begin());
    const uint64_t content_hash = fnv1a64(efi);
    for (unsigned i = 0; i < 16; ++i) {
        const uint64_t mixed =
            content_hash ^ (0x9e3779b97f4a7c15ULL * (i + 1));
        entries[16 + i] = static_cast<uint8_t>(mixed >> ((i % 8) * 8));
    }
    entries[16 + 7] = static_cast<uint8_t>(
        (entries[16 + 7] & 0x0fU) | 0x40U);
    entries[16 + 8] = static_cast<uint8_t>(
        (entries[16 + 8] & 0x3fU) | 0x80U);
    put64(entries, 32, partition_first_lba);
    put64(entries, 40, partition_last_lba);
    const std::string partition_name = "Sura EFI System";
    for (size_t i = 0; i < partition_name.size(); ++i) {
        put16(entries, 56 + i * 2,
              static_cast<uint8_t>(partition_name[i]));
    }
    const uint32_t entries_crc = crc32(entries.data(), entries.size());
    std::copy(entries.begin(), entries.end(),
              image.begin() + static_cast<std::ptrdiff_t>(2 * sector_size));
    std::copy(entries.begin(), entries.end(),
              image.begin() + static_cast<std::ptrdiff_t>(
                  backup_entries_lba * sector_size));

    uint8_t disk_guid[16]{};
    for (unsigned i = 0; i < 16; ++i) {
        const uint64_t mixed =
            (content_hash << (i % 13)) ^
            (0xd6e8feb86659fd93ULL * (i + 3));
        disk_guid[i] = static_cast<uint8_t>(mixed >> ((i % 8) * 8));
    }
    disk_guid[7] = static_cast<uint8_t>((disk_guid[7] & 0x0fU) | 0x40U);
    disk_guid[8] = static_cast<uint8_t>((disk_guid[8] & 0x3fU) | 0x80U);

    const auto write_gpt_header =
        [&](uint64_t header_lba, uint64_t alternate_lba,
            uint64_t table_lba) {
            std::vector<uint8_t> header(sector_size, 0);
            const char signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
            std::memcpy(header.data(), signature, sizeof(signature));
            put32(header, 8, 0x00010000);
            put32(header, 12, 92);
            put64(header, 24, header_lba);
            put64(header, 32, alternate_lba);
            put64(header, 40, 34);
            put64(header, 48, total_sectors - 34);
            std::memcpy(header.data() + 56, disk_guid, sizeof(disk_guid));
            put64(header, 72, table_lba);
            put32(header, 80, 128);
            put32(header, 84, 128);
            put32(header, 88, entries_crc);
            put32(header, 16, crc32(header.data(), 92));
            std::copy(
                header.begin(), header.end(),
                image.begin() + static_cast<std::ptrdiff_t>(
                    header_lba * sector_size));
        };
    write_gpt_header(1, backup_header_lba, 2);
    write_gpt_header(backup_header_lba, 1, backup_entries_lba);

    const size_t partition_offset =
        static_cast<size_t>(partition_first_lba * sector_size);
    // FAT32 BIOS parameter block. UEFI uses it as a filesystem, not as x86
    // boot code.
    image[partition_offset] = 0xeb;
    image[partition_offset + 1] = 0x58;
    image[partition_offset + 2] = 0x90;
    std::memcpy(image.data() + partition_offset + 3, "SURAOS  ", 8);
    put16(image, partition_offset + 11, sector_size);
    image[partition_offset + 13] = sectors_per_cluster;
    put16(image, partition_offset + 14, reserved_sectors);
    image[partition_offset + 16] = fat_count;
    put16(image, partition_offset + 17, 0);
    put16(image, partition_offset + 19, 0);
    image[partition_offset + 21] = 0xf8;
    put16(image, partition_offset + 22, 0);
    put16(image, partition_offset + 24, 63);
    put16(image, partition_offset + 26, 255);
    put32(image, partition_offset + 28,
          static_cast<uint32_t>(partition_first_lba));
    put32(image, partition_offset + 32,
          static_cast<uint32_t>(partition_sectors));
    put32(image, partition_offset + 36, fat_sectors);
    put16(image, partition_offset + 40, 0);
    put16(image, partition_offset + 42, 0);
    put32(image, partition_offset + 44, 2);
    put16(image, partition_offset + 48, 1);
    put16(image, partition_offset + 50, 6);
    image[partition_offset + 64] = 0x80;
    image[partition_offset + 66] = 0x29;
    put32(image, partition_offset + 67,
          static_cast<uint32_t>(content_hash));
    std::memcpy(image.data() + partition_offset + 71, "SURA EFI   ", 11);
    std::memcpy(image.data() + partition_offset + 82, "FAT32   ", 8);
    image[partition_offset + 510] = 0x55;
    image[partition_offset + 511] = 0xaa;
    std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(partition_offset),
                sector_size,
                image.begin() + static_cast<std::ptrdiff_t>(
                    partition_offset + 6 * sector_size));

    const uint64_t used_clusters = 3 + file_clusters;
    const auto write_fsinfo = [&](uint32_t relative_sector) {
        const size_t offset =
            partition_offset + static_cast<size_t>(relative_sector) * sector_size;
        put32(image, offset, 0x41615252);
        put32(image, offset + 484, 0x61417272);
        put32(image, offset + 488,
              static_cast<uint32_t>(cluster_count - used_clusters));
        put32(image, offset + 492,
              static_cast<uint32_t>(5 + file_clusters));
        put32(image, offset + 508, 0xaa550000);
    };
    write_fsinfo(1);
    write_fsinfo(7);

    const uint64_t fat_first_lba =
        partition_first_lba + reserved_sectors;
    const auto put_fat = [&](uint32_t cluster, uint32_t value) {
        for (uint32_t copy = 0; copy < fat_count; ++copy) {
            const uint64_t lba =
                fat_first_lba + static_cast<uint64_t>(copy) * fat_sectors;
            put32(image,
                  static_cast<size_t>(lba * sector_size) +
                      static_cast<size_t>(cluster) * 4,
                  value);
        }
    };
    put_fat(0, 0x0ffffff8);
    put_fat(1, 0x0fffffff);
    put_fat(2, 0x0fffffff);
    put_fat(3, 0x0fffffff);
    put_fat(4, 0x0fffffff);
    for (uint64_t i = 0; i < file_clusters; ++i) {
        const uint32_t cluster = static_cast<uint32_t>(5 + i);
        const uint32_t next = i + 1 == file_clusters
            ? 0x0fffffff
            : static_cast<uint32_t>(cluster + 1);
        put_fat(cluster, next);
    }

    const uint64_t data_first_lba =
        fat_first_lba + static_cast<uint64_t>(fat_count) * fat_sectors;
    const auto cluster_offset = [&](uint32_t cluster) -> size_t {
        return static_cast<size_t>(
            (data_first_lba +
             static_cast<uint64_t>(cluster - 2) * sectors_per_cluster) *
            sector_size);
    };

    write_short_entry(image, cluster_offset(2),
                      "EFI        ", 0x10, 3, 0);
    write_short_entry(image, cluster_offset(3),
                      ".          ", 0x10, 3, 0);
    write_short_entry(image, cluster_offset(3) + 32,
                      "..         ", 0x10, 2, 0);
    write_short_entry(image, cluster_offset(3) + 64,
                      "BOOT       ", 0x10, 4, 0);
    write_short_entry(image, cluster_offset(4),
                      ".          ", 0x10, 4, 0);
    write_short_entry(image, cluster_offset(4) + 32,
                      "..         ", 0x10, 3, 0);
    write_short_entry(image, cluster_offset(4) + 64,
                      "BOOTX64 EFI", 0x20, 5,
                      static_cast<uint32_t>(efi.size()));
    std::copy(efi.begin(), efi.end(),
              image.begin() + static_cast<std::ptrdiff_t>(cluster_offset(5)));

    return result;
}

} // namespace SuraUefiDisk

inline SuraUefiDiskResult
sura_build_uefi_disk_image(const std::vector<uint8_t>& efi) {
    return SuraUefiDisk::build(efi);
}

#endif
