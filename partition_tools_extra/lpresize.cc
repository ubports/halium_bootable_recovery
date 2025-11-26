// Copyright (C) 2025 UBPorts Foundation
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

#include <getopt.h>
#include <inttypes.h>
#include <unistd.h>

#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <fs_mgr_dm_linear.h>
#include <libdm/dm.h>
#include <liblp/builder.h>
#include <liblp/liblp.h>

using android::base::ParseByteCount;
using android::base::StringPrintf;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::fs_mgr::LpMetadata;
using android::fs_mgr::MetadataBuilder;
using android::fs_mgr::PartitionOpener;
using android::fs_mgr::ReadMetadata;
using android::fs_mgr::SlotNumberForSlotSuffix;
using android::fs_mgr::UpdatePartitionTable;

namespace {

[[noreturn]] void Usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [--slot=_a|_b] SUPER_PATH PARTITION_NAME NEW_SIZE\n\n"
            "  SUPER_PATH     Path to the super block device (e.g. /dev/block/by-name/super)\n"
            "  PARTITION_NAME Logical partition to resize (e.g. system_a)\n"
            "  NEW_SIZE       Target size in bytes (accepts decimal or suffixes like 4G)\n",
            prog);
    exit(EXIT_FAILURE);
}

std::optional<uint32_t> ParseSlotArgument(const std::string& slot_arg) {
    if (slot_arg.empty()) {
        return 0;
    }

    std::string suffix = slot_arg;
    if (suffix[0] != '_') {
        suffix = "_" + suffix;
    }

    if (suffix.size() != 2 || suffix[1] < 'a') {
        return std::nullopt;
    }

    uint32_t slot_number = static_cast<uint32_t>(suffix[1] - 'a');
    return slot_number;
}

bool ResizePartition(const std::string& super_path, const std::string& partition_name,
                     uint64_t new_size, uint32_t slot_number) {
    auto& dm = DeviceMapper::Instance();
    bool was_mapped = dm.GetState(partition_name) != DmDeviceState::INVALID;
    if (was_mapped) {
        LOG(INFO) << "Logical partition " << partition_name << " is mapped, unmapping";
        if (!android::fs_mgr::DestroyLogicalPartition(partition_name)) {
            LOG(ERROR) << "Failed to unmap logical partition " << partition_name;
            return false;
        }
    }

    auto metadata = ReadMetadata(super_path, slot_number);
    if (!metadata) {
        LOG(ERROR) << "Failed to read metadata for " << super_path;
        return false;
    }

    auto builder = MetadataBuilder::New(*metadata.get());
    if (!builder) {
        LOG(ERROR) << "Failed to create metadata builder";
        return false;
    }

    auto partition = builder->FindPartition(partition_name);
    if (!partition) {
        LOG(ERROR) << "Partition " << partition_name << " not found";
        return false;
    }

    uint32_t attrs = partition->attributes();
    partition->set_attributes(attrs & ~LP_PARTITION_ATTR_UPDATED);

    if (!builder->ResizePartition(partition, new_size)) {
        LOG(ERROR) << "Not enough space to resize partition";
        return false;
    }

    auto new_metadata = builder->Export();
    if (!new_metadata) {
        LOG(ERROR) << "Failed to export updated metadata";
        return false;
    }

    PartitionOpener opener;
    uint32_t slots = std::min(new_metadata->geometry.metadata_slot_count, static_cast<uint32_t>(2));
    if (slots == 0) {
        slots = 1;
    }

    for (uint32_t i = 0; i < slots; i++) {
        if (!UpdatePartitionTable(opener, super_path, *new_metadata.get(), i)) {
            LOG(ERROR) << "Failed to write metadata for slot " << i;
            return false;
        }
    }

    if (was_mapped) {
        LOG(INFO) << "Remapping logical partition " << partition_name;
        android::fs_mgr::CreateLogicalPartitionParams params;
        params.block_device = super_path;
        params.metadata = new_metadata.get();
        params.partition_name = partition_name;
        params.metadata_slot = slot_number;
        params.force_writable = true;

        std::string new_path;
        if (!android::fs_mgr::CreateLogicalPartition(params, &new_path)) {
            LOG(ERROR) << "Failed to remap logical partition " << partition_name;
            return false;
        }
    }

    return true;
}

}  // namespace

int main(int argc, char** argv) {
    android::base::InitLogging(argv, &android::base::StderrLogger);

    static struct option kLongOptions[] = {
            {"slot", required_argument, nullptr, 's'},
            {"help", no_argument, nullptr, 'h'},
            {nullptr, 0, nullptr, 0},
    };

    std::string slot_suffix;

    int opt;
    while ((opt = getopt_long(argc, argv, "", kLongOptions, nullptr)) != -1) {
        switch (opt) {
            case 's':
                slot_suffix = optarg ? optarg : "";
                break;
            case 'h':
            default:
                Usage(argv[0]);
        }
    }

    if (argc - optind != 3) {
        Usage(argv[0]);
    }

    std::string super_path = argv[optind++];
    std::string partition_name = argv[optind++];
    std::string size_arg = argv[optind++];

    uint64_t new_size = 0;
    if (!ParseByteCount(size_arg, &new_size)) {
        LOG(ERROR) << "Invalid size: " << size_arg;
        return EXIT_FAILURE;
    }

    auto slot_number = ParseSlotArgument(slot_suffix);
    if (!slot_number) {
        LOG(ERROR) << "Invalid slot suffix: " << slot_suffix;
        return EXIT_FAILURE;
    }

    if (!ResizePartition(super_path, partition_name, new_size, *slot_number)) {
        return EXIT_FAILURE;
    }

    std::cout << StringPrintf("Resized %s to %" PRIu64 " bytes", partition_name.c_str(), new_size)
              << std::endl;
    return EXIT_SUCCESS;
}
