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

#pragma once

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

// LVM uses 1MB extents (2048 x 512-byte sectors).
static constexpr uint64_t kLvmExtentSizeSectors = 2048;
static constexpr uint64_t kLvmExtentSizeBytes = kLvmExtentSizeSectors * 512;

// Temporary metadata file used during migration.
static constexpr const char* kMetadataPath = "/tmp/lvm_migrator_vgcfg.txt";

// Backup of the trailing 1MB of a filesystem lvm-fs-migrator could not
// shrink (tail relocation); the orchestrator writes it back after
// extending the LV past the original partition size.
static inline std::string TailBackupPath(const std::string& lv_name) {
    return "/tmp/lvm-migrate-tail-" + lv_name + ".bin";
}

// Generate an LVM UUID in XXXXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXXXXX format
// (6-4-4-4-4-4-6 groups of uppercase hex, 32 chars + 6 hyphens = 38 chars).
static inline std::string GenerateId() {
    uint8_t bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return "";
    if (read(fd, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) {
        close(fd);
        return "";
    }
    close(fd);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(&hex[i * 2], 3, "%02X", bytes[i]);
    static const int groups[] = {6, 4, 4, 4, 4, 4, 6};
    std::string result;
    int pos = 0;
    for (int g = 0; g < 7; g++) {
        if (g > 0) result += '-';
        result.append(hex + pos, groups[g]);
        pos += groups[g];
    }
    return result;
}

static inline int RunCommand(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    if (pid < 0) return -1;

    int status;
    if (TEMP_FAILURE_RETRY(waitpid(pid, &status, 0)) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static inline uint64_t GetDeviceSize(const std::string& device_path) {
    int fd = open(device_path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    uint64_t size = 0;
    ioctl(fd, BLKGETSIZE64, &size);
    close(fd);
    return size;
}

// Resolve all symlinks in path to get the canonical block device node.
// LVM tools enumerate devices from /sys/block and do not follow symlinks, so
// by-name paths like /dev/block/bootdevice/by-name/userdata must be resolved
// to their real device node (e.g. /dev/block/sda50) before calling pvcreate.
static inline std::string ResolvePath(const std::string& path) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved)) return resolved;
    return path;
}

// Check whether a volume group with the given name already exists.
static inline bool VolumeGroupExists(const std::string& vg_name) {
    return RunCommand({"vgs", "--noheadings", vg_name}) == 0;
}

// The ext2/3/4 superblock begins 1024 bytes into the partition.
static constexpr uint64_t kExt4SuperblockOffset = 1024;

// Run pvcreate + vgcfgrestore + vgchange to bring up the volume group.
// device_path must be a resolved (non-symlink) block device path; use
// ResolvePath() before calling this function.
static inline bool CreateAndActivateLvm(const std::string& device_path,
                                        const std::string& vg_name,
                                        const std::string& pv_id,
                                        const std::string& metadata_path) {
    if (RunCommand({"pvcreate", "--yes", "--uuid", pv_id, "--restorefile", metadata_path,
                    device_path}) != 0) {
        std::cerr << "pvcreate failed\n";
        return false;
    }
    // Refresh LVM's in-memory device list so vgcfgrestore can find the new PV.
    RunCommand({"vgscan"});
    if (RunCommand({"vgcfgrestore", "-f", metadata_path, vg_name}) != 0) {
        std::cerr << "vgcfgrestore failed\n";
        return false;
    }
    if (RunCommand({"vgchange", "-ay", vg_name}) != 0) {
        std::cerr << "vgchange failed\n";
        return false;
    }
    return true;
}

// Build one physical_volume block for the LVM metadata text format.
static inline std::string BuildPvBlock(const std::string& pv_name,
                                        const std::string& pv_id,
                                        const std::string& device_path,
                                        uint64_t total_extents) {
    std::string s;
    s += "        " + pv_name + " {\n";
    s += "            id = \"" + pv_id + "\"\n";
    s += "            device = \"" + device_path + "\"\n";
    s += "            status = [\"ALLOCATABLE\"]\n";
    s += "            flags = []\n";
    s += "            dev_size = " + std::to_string(total_extents * kLvmExtentSizeSectors) + "\n";
    s += "            pe_start = " + std::to_string(kLvmExtentSizeSectors) + "\n";
    s += "            pe_count = " + std::to_string(total_extents) + "\n";
    s += "        }\n";
    return s;
}

// Build the common LVM VG/PV header block (everything before logical_volumes).
static inline std::string BuildLvmHeader(const std::string& vg_name,
                                          const std::string& vg_id,
                                          const std::string& pv_id,
                                          const std::string& device_path,
                                          uint64_t total_extents,
                                          time_t creation_time) {
    std::string s;
    s += "contents = \"Text Format Volume Group\"\n";
    s += "version = 1\n";
    s += "description = \"Generated by lvm_migrator\"\n";
    s += "creation_host = \"localhost\"\n";
    s += "creation_time = " + std::to_string(creation_time) + "\n\n";
    s += vg_name + " {\n";
    s += "    id = \"" + vg_id + "\"\n";
    s += "    seqno = 0\n";
    s += "    format = \"lvm2\"\n";
    s += "    status = [\"READ\", \"WRITE\", \"RESIZEABLE\"]\n";
    s += "    flags = []\n";
    s += "    extent_size = " + std::to_string(kLvmExtentSizeSectors) + "\n";
    s += "    max_lv = 0\n";
    s += "    max_pv = 0\n";
    s += "    metadata_copies = 0\n\n";
    s += "    physical_volumes {\n";
    s += BuildPvBlock("pv0", pv_id, device_path, total_extents);
    s += "    }\n\n";
    return s;
}

// Build the logical_volume block for a filesystem-origin LV (two segments:
// header copy right after the data + main data region at the start).
static inline std::string BuildFsLvBlock(const std::string& lv_name,
                                          const std::string& lv_id,
                                          const std::string& pv_name,
                                          uint64_t main_data_extents,
                                          time_t creation_time) {
    std::string s;
    s += "        " + lv_name + " {\n";
    s += "            id = \"" + lv_id + "\"\n";
    s += "            status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n";
    s += "            flags = []\n";
    s += "            creation_time = " + std::to_string(creation_time) + "\n";
    s += "            creation_host = \"localhost\"\n";
    s += "            segment_count = 2\n\n";
    s += "            segment1 {\n";
    s += "                start_extent = 0\n";
    s += "                extent_count = 1\n";
    s += "                type = \"striped\"\n";
    s += "                stripe_count = 1\n";
    s += "                stripes = [\n";
    s += "                    \"" + pv_name + "\", " + std::to_string(main_data_extents) + "\n";
    s += "                ]\n";
    s += "            }\n";
    s += "            segment2 {\n";
    s += "                start_extent = 1\n";
    s += "                extent_count = " + std::to_string(main_data_extents) + "\n";
    s += "                type = \"striped\"\n";
    s += "                stripe_count = 1\n";
    s += "                stripes = [\n";
    s += "                    \"" + pv_name + "\", 0\n";
    s += "                ]\n";
    s += "            }\n";
    s += "        }\n";
    return s;
}
