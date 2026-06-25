// Copyright (C) 2026 UBports Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Migrates an ext4 partition to an LVM physical volume in-place.
//
// Layout after migration:
//
//   [ LVM metadata 1MB ][ ext4 data ][ ext4 superblock copy 1MB ][ reserved ]
//
// The logical volume exposes extents in this order:
//   PE(main_data_extents) — the superblock copy (LV offset 0 = original
//                           filesystem start)
//   PE(0..main_data_extents-1) — the ext4 data
//
// This preserves the original ext4 superblock at LV sector 0 so the
// filesystem can be mounted without modification.

#include "lvm_migrator_utils.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include <android-base/file.h>

// Whether resize2fs can shrink the filesystem to target_bytes, based on
// its estimated minimum size (resize2fs -P). On any estimation failure
// assume it can, preserving the previous try-and-see behavior.
static bool CanShrinkTo(const std::string& device_path, uint64_t target_bytes) {
    // Filesystem block size from the superblock: 1024 << s_log_block_size.
    uint32_t log_block_size = 0;
    int fd = open(device_path.c_str(), O_RDONLY);
    if (fd < 0) return true;
    ssize_t n = pread(fd, &log_block_size, sizeof(log_block_size),
                      kExt4SuperblockOffset + 24);
    close(fd);
    if (n != (ssize_t)sizeof(log_block_size) || log_block_size > 6)
        return true;
    uint64_t block_size = 1024ULL << log_block_size;

    std::string cmd = "resize2fs -P " + device_path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) return true;
    // "Estimated minimum size of the filesystem: 123456"
    char buf[256];
    uint64_t min_blocks = 0;
    bool found = false;
    while (fgets(buf, sizeof(buf), p) != nullptr) {
        const char* colon = strrchr(buf, ':');
        if (colon != nullptr) {
            char* end = nullptr;
            uint64_t v = strtoull(colon + 1, &end, 10);
            if (end != colon + 1) {
                min_blocks = v;
                found = true;
            }
        }
    }
    pclose(p);
    if (!found) return true;

    uint64_t min_bytes = min_blocks * block_size;
    std::cout << "Minimum filesystem size: " << min_bytes << " bytes\n";
    return min_bytes <= target_bytes;
}

// Save the 1MB at `offset` (about to be overwritten by the relocated
// header) so the orchestrator can write it back after extending the LV.
static bool BackupTail(const std::string& device_path, uint64_t offset,
                       const std::string& path) {
    std::vector<uint8_t> buf(kLvmExtentSizeBytes);
    int fd = open(device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open " << device_path << ": "
                  << strerror(errno) << "\n";
        return false;
    }
    if (pread(fd, buf.data(), buf.size(), offset) != (ssize_t)buf.size()) {
        std::cerr << "Failed to read filesystem tail: " << strerror(errno)
                  << "\n";
        close(fd);
        return false;
    }
    close(fd);

    int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0) {
        std::cerr << "Failed to create " << path << ": " << strerror(errno)
                  << "\n";
        return false;
    }
    bool ok = write(out, buf.data(), buf.size()) == (ssize_t)buf.size();
    fsync(out);
    close(out);
    if (!ok) std::cerr << "Failed to write " << path << "\n";
    return ok;
}

static bool CopyHeader(const std::string& device_path, uint64_t src_offset,
                       uint64_t dst_offset) {
    std::vector<uint8_t> buf(kLvmExtentSizeBytes);
    int fd = open(device_path.c_str(), O_RDWR);
    if (fd < 0) {
        std::cerr << "Failed to open " << device_path << ": " << strerror(errno) << "\n";
        return false;
    }
    if (pread(fd, buf.data(), buf.size(), src_offset) != (ssize_t)buf.size()) {
        std::cerr << "Failed to read header: " << strerror(errno) << "\n";
        close(fd);
        return false;
    }
    if (pwrite(fd, buf.data(), buf.size(), dst_offset) != (ssize_t)buf.size()) {
        std::cerr << "Failed to write header copy: " << strerror(errno) << "\n";
        close(fd);
        return false;
    }
    fsync(fd);
    close(fd);
    return true;
}

static std::string BuildMetadata(const std::string& vg_name, const std::string& lv_name,
                                  const std::string& device_path, uint64_t total_extents,
                                  uint64_t main_data_extents, time_t creation_time,
                                  const std::string& vg_id, const std::string& pv_id,
                                  const std::string& lv_id) {
    std::string s = BuildLvmHeader(vg_name, vg_id, pv_id, device_path, total_extents,
                                    creation_time);
    s += "    logical_volumes {\n";
    s += BuildFsLvBlock(lv_name, lv_id, "pv0", main_data_extents, creation_time);
    s += "    }\n";
    s += "}\n";
    return s;
}

int main(int argc, char* argv[]) {
    std::string device_path, vg_name, lv_name;
    uint64_t reserved_size_mb = 0;
    bool allow_tail_relocate = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "Usage: lvm-fs-migrator --device=<dev> --vg-name=<name>"
                         " --lv-name=<name> [--reserve=<mb>]"
                         " [--allow-tail-relocate]\n";
            return 0;
        } else if (arg == "--allow-tail-relocate") {
            allow_tail_relocate = true;
        } else if (arg.rfind("--device=", 0) == 0) {
            device_path = arg.substr(9);
        } else if (arg.rfind("--vg-name=", 0) == 0) {
            vg_name = arg.substr(10);
        } else if (arg.rfind("--lv-name=", 0) == 0) {
            lv_name = arg.substr(10);
        } else if (arg.rfind("--reserve=", 0) == 0) {
            const std::string val = arg.substr(10);
            if (val.empty() || val.find_first_not_of("0123456789") != std::string::npos) {
                std::cerr << "Invalid --reserve value: " << val << "\n";
                return 1;
            }
            reserved_size_mb = strtoull(val.c_str(), nullptr, 10);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    if (device_path.empty() || vg_name.empty() || lv_name.empty()) {
        std::cerr << "Error: --device, --vg-name, and --lv-name are required\n";
        return 1;
    }

    // LVM enumerates devices from /sys/block and does not follow symlinks, so
    // resolve by-name paths to real block device nodes before any LVM call.
    device_path = ResolvePath(device_path);

    struct stat st;
    if (stat(device_path.c_str(), &st) != 0 || !S_ISBLK(st.st_mode)) {
        std::cerr << "Error: " << device_path << " is not a block device\n";
        return 1;
    }

    if (VolumeGroupExists(vg_name)) {
        std::cerr << "Error: volume group '" << vg_name << "' already exists\n";
        return 1;
    }

    // normal single-PV mode

    uint64_t device_size = GetDeviceSize(device_path);
    if (device_size == 0) {
        std::cerr << "Failed to get device size for " << device_path << "\n";
        return 1;
    }
    std::cout << "Device size: " << device_size << " bytes ("
              << device_size / kLvmExtentSizeBytes << " MB)\n";

    // One extent goes to the LVM metadata, one to the header copy; the
    // reserve comes on top of those, so anything larger underflows the
    // size math below.
    if (device_size < (reserved_size_mb + 2) * kLvmExtentSizeBytes) {
        std::cerr << "Error: --reserve=" << reserved_size_mb
                  << " MB does not fit on a " << device_size / kLvmExtentSizeBytes
                  << " MB device\n";
        return 1;
    }

    // All sizes rounded down to 1MB (one LVM extent) boundaries.
    uint64_t reserved_bytes = reserved_size_mb * kLvmExtentSizeBytes;
    uint64_t filesystem_size_mb =
            (device_size - kLvmExtentSizeBytes - reserved_bytes) / kLvmExtentSizeBytes;
    uint64_t filesystem_size = filesystem_size_mb * kLvmExtentSizeBytes;

    // total_extents = device area after the 1MB LVM metadata region.
    uint64_t total_extents = (device_size - kLvmExtentSizeBytes) / kLvmExtentSizeBytes;
    // 1 extent reserved for header copy at the end.
    uint64_t main_data_extents = total_extents - 1 - reserved_size_mb;

    std::cout << "Checking filesystem on " << device_path << "...\n";
    int fsck = RunCommand({"e2fsck", "-f", "-y", device_path});
    if (fsck != 0 && fsck != 1) {
        std::cerr << "e2fsck failed with code " << fsck << "\n";
        return 1;
    }

    if (!CanShrinkTo(device_path, filesystem_size)) {
        if (!allow_tail_relocate || reserved_size_mb > 0) {
            std::cerr << "Filesystem cannot shrink to " << filesystem_size_mb
                      << " MB; aborting\n";
            return 1;
        }
        // The filesystem cannot give up even the single MB that LVM
        // metadata needs. Back up the trailing 1MB (about to be overwritten
        // by the relocated header) and leave the filesystem at full size;
        // the orchestrator extends the LV past the original partition size
        // and restores the tail there.
        std::string tail_path = TailBackupPath(lv_name);
        std::cout << "Filesystem too full to shrink; backing up trailing"
                     " 1MB to " << tail_path << "\n";
        if (!BackupTail(device_path, filesystem_size, tail_path)) return 1;
    } else {
        std::cout << "Resizing filesystem to " << filesystem_size_mb
                  << " MB...\n";
        if (RunCommand({"resize2fs", "-f", device_path,
                        std::to_string(filesystem_size_mb) + "M"}) != 0) {
            std::cerr << "resize2fs failed\n";
            return 1;
        }
    }

    // Copy the 1MB superblock to the end of the filesystem area so LVM can
    // use the start of the device for its own metadata
    std::cout << "Copying 1MB header to offset " << filesystem_size << "...\n";
    if (!CopyHeader(device_path, 0, filesystem_size)) return 1;

    std::string vg_id = GenerateId();
    std::string pv_id = GenerateId();
    std::string lv_id = GenerateId();
    time_t creation_time = time(nullptr);

    std::string metadata = BuildMetadata(vg_name, lv_name, device_path, total_extents,
                                          main_data_extents, creation_time,
                                          vg_id, pv_id, lv_id);

    if (!android::base::WriteStringToFile(metadata, kMetadataPath)) {
        std::cerr << "Failed to write metadata file\n";
        return 1;
    }

    std::cout << "Creating LVM volume group '" << vg_name << "'...\n";
    bool ok = CreateAndActivateLvm(device_path, vg_name, pv_id, kMetadataPath);
    unlink(kMetadataPath);

    if (!ok) return 1;

    std::cout << "Migration successful: " << vg_name << "/" << lv_name << "\n";
    return 0;
}
