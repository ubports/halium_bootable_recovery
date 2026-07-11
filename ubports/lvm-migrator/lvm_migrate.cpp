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

// Boot-time LVM migration orchestrator.
//
// Inspects the current state (VG, LV, PV, partition layout) and determines
// the migration steps needed to reach the target LVM configuration. Delegates
// actual migration work to lvm-fs-migrator and raw LVM tools.
//
// Any unexpected state (orphaned PVs, corrupted metadata, partial prior runs)
// is treated as a hard error; it does not attempt automatic recovery.
//
// Pass --dry-run (-n) to inspect the device state and log the commands that
// would be executed without modifying anything.

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <cutils/partition_utils.h>

#include "lvm_migrator_utils.h"
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

// LVM uses 1MB extents.
static constexpr uint64_t kExtentSizeBytes = 1048576;

// In dry-run mode all state inspection runs normally but every mutating
// command is logged instead of executed.
static bool g_dry_run = false;

// In check mode (implies dry-run) the exit code reports whether migration
// work is pending: 0 = nothing to do, 2 = work pending, 1 = error. Pending
// work is detected by counting the mutating commands a real run would
// execute, because some plans (e.g. an existing VG whose rootfs still needs
// lvextend) end in Strategy::DONE despite requiring work.
static bool g_check_only = false;
static int g_pending_mutations = 0;

// Log to stderr and, when available, /dev/kmsg.
static int g_kmsg_fd = -1;

static void Log(const std::string& msg) {
    std::cerr << msg << "\n" << std::flush;
    if (g_kmsg_fd >= 0) {
        std::string line = msg + "\n";
        ssize_t n = write(g_kmsg_fd, line.c_str(), line.size());
        (void)n;
    }
}

// Run a command and return its exit code.
static int RunCmd(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    if (pid < 0) return -1;
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static std::string JoinCmd(const std::vector<std::string>& args) {
    return android::base::Join(args, ' ');
}

// Run a command that modifies device state. In dry-run mode the command is
// logged and reported as successful without being executed. Commands that
// merely (re-)establish runtime state on an already-migrated device (such
// as activating an existing VG) pass count_mutation=false so check mode
// does not report them as pending work.
static int RunOrLog(const std::vector<std::string>& args,
                    bool count_mutation = true) {
    if (g_dry_run) {
        if (count_mutation) g_pending_mutations++;
        Log("lvm-migrate: [dry-run] would run: " + JoinCmd(args));
        return 0;
    }
    return RunCmd(args);
}

// Run a command and capture its stdout. Returns empty string on any error.
static std::string RunCmdOutput(const std::vector<std::string>& args) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return "";

    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) {
        close(pipefd[0]);
        return "";
    }

    std::string result;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        result.append(buf, static_cast<size_t>(n));
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return "";

    // Trim trailing whitespace.
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

static bool BlockDeviceExists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

static std::string Resolve(const std::string& path) {
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf)) return buf;
    return path;
}

static uint64_t GetSize(const std::string& path) {
    std::string real = Resolve(path);
    int fd = open(real.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    uint64_t size = 0;
    ioctl(fd, BLKGETSIZE64, &size);
    close(fd);
    return size;
}

static bool VgExists(const std::string& vg) {
    return RunCmd({"vgs", "--noheadings", vg}) == 0;
}

static uint64_t GetLvSize(const std::string& vg, const std::string& lv) {
    std::string s = RunCmdOutput({
        "lvs", "--noheadings", "--units", "B", "--nosuffix",
        "-o", "lv_size", vg + "/" + lv});
    if (s.empty()) return 0;
    return strtoull(s.c_str(), nullptr, 10);
}

static uint64_t GetVgFree(const std::string& vg) {
    std::string s = RunCmdOutput({
        "vgs", "--noheadings", "--units", "B", "--nosuffix",
        "-o", "vg_free", vg});
    if (s.empty()) return 0;
    return strtoull(s.c_str(), nullptr, 10);
}

// Contents of a raw partition, used to decide between preserving and
// recreating it. BLANK is only reported for a partition that fs_mgr's
// wipe detection considers erased (all zeros or all ones, e.g. after
// "fastboot erase"); unrecognized non-blank contents stay UNKNOWN so user
// data in an unexpected format is never sacrificed.
enum class Content { EXT4, PV, BLANK, UNKNOWN };

static const char* ContentName(Content c) {
    switch (c) {
        case Content::EXT4: return "ext4";
        case Content::PV: return "lvm-pv";
        case Content::BLANK: return "blank";
        default: return "unknown";
    }
}

static Content ProbeContent(const std::string& dev) {
    std::string real = Resolve(dev);

    int fd = open(real.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Content::UNKNOWN;

    // The LVM PV label "LABELONE" sits in one of the first four sectors.
    char label[8];
    Content c = Content::UNKNOWN;
    for (int s = 0; s < 4 && c == Content::UNKNOWN; s++) {
        if (pread(fd, label, sizeof(label), s * 512) ==
                    (ssize_t)sizeof(label) &&
            memcmp(label, "LABELONE", sizeof(label)) == 0)
            c = Content::PV;
    }

    // ext4 superblock magic at offset 1024 + 56.
    unsigned char magic[2];
    if (c == Content::UNKNOWN && pread(fd, magic, 2, 1080) == 2 &&
        magic[0] == 0x53 && magic[1] == 0xEF)
        c = Content::EXT4;

    close(fd);

    if (c == Content::UNKNOWN && partition_wiped(real.c_str()))
        c = Content::BLANK;

    return c;
}

// Number of PVs the VG metadata references that cannot be found, e.g.
// after a member partition was erased from fastboot.
static uint64_t GetVgMissingPvCount(const std::string& vg) {
    std::string s = RunCmdOutput({"vgs", "--noheadings",
                                  "-o", "vg_missing_pv_count", vg});
    if (s.empty()) return 0;
    return strtoull(s.c_str(), nullptr, 10);
}

// Destroy LVM/filesystem signatures at the start of a partition so it can
// be recreated from scratch. Only used on the sacrifice path.
static bool WipeStart(const std::string& dev) {
    if (g_dry_run) {
        g_pending_mutations++;
        Log("lvm-migrate: [dry-run] would wipe first 1MB of " + dev);
        return true;
    }
    int fd = open(Resolve(dev).c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::vector<char> zeros(kExtentSizeBytes, 0);
    bool ok = pwrite(fd, zeros.data(), zeros.size(), 0) ==
              (ssize_t)zeros.size();
    fsync(fd);
    close(fd);
    return ok;
}

struct Layout {
    bool has_super = false;
    bool is_ab = false;

    std::string userdata;       // <by-name dir>/userdata
    std::string system_a;
    std::string system_b;
    std::string system_single;
    std::string active_slot;

    Content userdata_content = Content::BLANK;
    Content system_content = Content::BLANK;
};

// The by-name directory location varies: most devices expose
// /dev/block/by-name, while Qualcomm ones use a bootdevice symlink.
// Wait up to ~2s for ueventd to create the userdata symlink so we neither
// race symlink creation nor misdetect the layout, and return the first
// directory that contains it.
static std::string FindByNameDir() {
    static const char* kCandidates[] = {
        "/dev/block/by-name",
        "/dev/block/bootdevice/by-name",
        "/dev/block/platform/bootdevice/by-name",
    };
    for (int i = 0; i < 20; i++) {
        for (const char* base : kCandidates) {
            if (BlockDeviceExists(std::string(base) + "/userdata")) return base;
        }
        usleep(100000);
    }
    return "";
}

static Layout DetectLayout() {
    Layout l;

    // userdata is present on every device; its location determines the
    // by-name directory for all other partitions.
    std::string b = FindByNameDir();
    if (b.empty()) {
        Log("lvm-migrate: no by-name directory with userdata found");
        return l;
    }
    Log("lvm-migrate: using partition directory " + b);

    l.has_super = BlockDeviceExists(std::string(b) + "/super");

    auto exists = [&](const char* name) {
        std::string p = std::string(b) + "/" + name;
        return BlockDeviceExists(p) ? p : std::string();
    };

    l.system_a = exists("system_a");
    l.system_b = exists("system_b");
    l.system_single = exists("system");
    l.userdata = exists("userdata");
    l.is_ab = !l.system_a.empty() && !l.system_b.empty();
    l.active_slot = android::base::GetProperty("ro.boot.slot_suffix", "");

    if (!l.userdata.empty()) l.userdata_content = ProbeContent(l.userdata);
    if (!l.system_single.empty())
        l.system_content = ProbeContent(l.system_single);

    return l;
}

enum class Strategy {
    DONE,
    AB_SLOT_MERGE,
    NONAB_USERDATA_AND_SYSTEM,
    NONAB_GROW_ROOTFS,
    NONAB_CREATE_ROOTFS,
    ERROR,
};

struct Plan {
    Strategy strategy = Strategy::ERROR;
    std::string src;       // system source (by-name)
    std::string spare;     // system spare (by-name)
    uint64_t reserve_mb = 0;

    // An erased partition is recreated inside the VG instead of being
    // migrated in place.
    bool userdata_fresh = false;
    bool system_fresh = false;
    // Sacrifice path: wipe a stale PV label off system and deactivate the
    // broken VG before rebuilding.
    bool wipe_system = false;
    bool deactivate_first = false;
};

// The VG exists with all PVs present: verify it matches the target state,
// growing rootfs from VG free space when possible.
static Plan PlanForExistingVg(const Layout& l, const std::string& vg,
                              uint64_t required_rootfs_mb) {
    Plan p;

    Log("lvm-migrate: VG " + vg + " exists, activating");
    RunOrLog({"vgchange", "-ay", vg}, /*count_mutation=*/false);

    if (l.is_ab && !l.has_super) {
        uint64_t lv_bytes = GetLvSize(vg, "rootfs");
        uint64_t required = required_rootfs_mb * kExtentSizeBytes;

        if (lv_bytes >= required) {
            p.strategy = Strategy::DONE;
            return p;
        }

        if (lv_bytes > 0) {
            uint64_t free = GetVgFree(vg);
            if (lv_bytes + free >= required) {
                Log("lvm-migrate: extending rootfs from "
                     + std::to_string(lv_bytes / kExtentSizeBytes)
                     + " MB to " + std::to_string(required_rootfs_mb) + " MB");
                RunOrLog({"lvextend", "-L",
                        std::to_string(required_rootfs_mb) + "M",
                        vg + "/rootfs"});
                RunOrLog({"resize2fs", "-f", "/dev/" + vg + "/rootfs"});
                p.strategy = Strategy::DONE;
                return p;
            }
        }

        Log("lvm-migrate: rootfs LV is " + std::to_string(lv_bytes)
              + " bytes, need " + std::to_string(required)
              + ", only " + std::to_string(GetVgFree(vg)) + " bytes free");
        p.strategy = Strategy::ERROR;
        return p;
    }

    uint64_t userdata_bytes = GetLvSize(vg, "userdata");
    if (userdata_bytes == 0) {
        Log("lvm-migrate: VG " + vg + " exists but userdata LV is missing");
        p.strategy = Strategy::ERROR;
        return p;
    }

    // A raised size requirement (a newer image needing a bigger rootfs)
    // must grow the rootfs LV, taking the space back from userdata when
    // the VG has none free.
    uint64_t rootfs_bytes = GetLvSize(vg, "rootfs");
    if (rootfs_bytes == 0) {
        // An earlier run migrated userdata but died before rootfs was
        // created; system holds nothing worth preserving at this point.
        if (l.system_single.empty() || l.has_super) {
            Log("lvm-migrate: VG " + vg + " exists but rootfs LV is missing");
            p.strategy = Strategy::ERROR;
            return p;
        }
        Log("lvm-migrate: VG " + vg + " exists but rootfs LV is missing;"
            " creating it on " + l.system_single);
        p.src = l.system_single;
        p.wipe_system = (l.system_content == Content::PV);
        p.strategy = Strategy::NONAB_CREATE_ROOTFS;
        return p;
    }
    if (rootfs_bytes < required_rootfs_mb * kExtentSizeBytes) {
        Log("lvm-migrate: rootfs is "
            + std::to_string(rootfs_bytes / kExtentSizeBytes) + " MB, needs "
            + std::to_string(required_rootfs_mb) + " MB");
        p.strategy = Strategy::NONAB_GROW_ROOTFS;
        return p;
    }

    p.strategy = Strategy::DONE;
    return p;
}

static Plan MakePlan(const Layout& l, const std::string& vg,
                      uint64_t required_rootfs_mb) {
    Plan p;

    // VG already exists: verify it matches the expected state.
    if (VgExists(vg)) {
        uint64_t missing = GetVgMissingPvCount(vg);
        if (missing == 0) return PlanForExistingVg(l, vg, required_rootfs_mb);

        // A member partition was erased underneath the VG (e.g.
        // "fastboot erase" during a new installation).
        if (l.userdata_content == Content::PV) {
            // The userdata PV is intact, so the missing PV is another
            // member (erased system?). Refuse to touch anything so the
            // userdata contents can still be recovered manually.
            Log("lvm-migrate: VG " + vg + " is missing "
                + std::to_string(missing) + " PV(s) but the userdata PV"
                  " is intact; refusing to rebuild so its data can be"
                  " recovered manually");
            p.strategy = Strategy::ERROR;
            return p;
        }
        if (l.userdata_content == Content::UNKNOWN) {
            Log("lvm-migrate: VG " + vg + " is missing PV(s) and userdata"
                " contents are unrecognized; not touching them");
            p.strategy = Strategy::ERROR;
            return p;
        }
        if (l.has_super || l.is_ab) {
            Log("lvm-migrate: broken VG rebuild not supported on this"
                " layout");
            p.strategy = Strategy::ERROR;
            return p;
        }
        // userdata itself was erased or reformatted, so the VG protects
        // nothing anymore; deactivate it and continue with the normal
        // planning below, which sacrifices the remaining PVs (rootfs is
        // reinstalled by the OTA).
        Log("lvm-migrate: VG " + vg + " is missing "
            + std::to_string(missing) + " PV(s) and userdata is "
            + ContentName(l.userdata_content) + "; rebuilding");
        p.deactivate_first = true;
    }

    // A/B without super: system-only migration.
    if (!l.has_super && l.is_ab) {
        if (l.active_slot == "_a") {
            p.src = l.system_a;
            p.spare = l.system_b;
        } else {
            p.src = l.system_b;
            p.spare = l.system_a;
        }
        uint64_t src_mb = GetSize(p.src) / kExtentSizeBytes;
        if (src_mb >= required_rootfs_mb) {
            Log("lvm-migrate: source " + p.src + " is already "
                + std::to_string(src_mb) + " MB (need "
                + std::to_string(required_rootfs_mb) + "), nothing to do");
            p.strategy = Strategy::DONE;
            return p;
        }
        p.strategy = Strategy::AB_SLOT_MERGE;
        return p;
    }

    // Non-A/B without super: userdata + system.
    if (!l.has_super && !l.system_single.empty() && !l.userdata.empty()) {
        p.src = l.system_single;
        p.strategy = Strategy::NONAB_USERDATA_AND_SYSTEM;

        // Only a wiped userdata is recreated. Unrecognized non-blank
        // contents might be user data in an unexpected format, and an
        // orphaned PV label (its VG is no longer visible) may still be
        // recoverable by hand; neither is ever sacrificed.
        if (l.userdata_content == Content::UNKNOWN) {
            Log("lvm-migrate: userdata contents are unrecognized;"
                " refusing to touch them");
            p.strategy = Strategy::ERROR;
            return p;
        }
        if (l.userdata_content == Content::PV) {
            Log("lvm-migrate: userdata has an orphaned PV label;"
                " not proceeding");
            p.strategy = Strategy::ERROR;
            return p;
        }
        p.userdata_fresh = (l.userdata_content == Content::BLANK);

        // system never holds user data and the OTA reinstalls rootfs, so
        // anything but a healthy ext4 (stock ROM filesystems, erased, a
        // stale PV label from a broken VG) is recreated from scratch.
        p.system_fresh = (l.system_content != Content::EXT4);
        p.wipe_system = (l.system_content == Content::PV);
        if (p.system_fresh && l.system_content != Content::BLANK)
            Log("lvm-migrate: system contents are "
                + std::string(ContentName(l.system_content))
                + "; recreating rootfs from scratch");

        // Rootfs after in-place migration is 1 MB smaller than the raw
        // partition (LVM metadata). If that is not enough we must borrow
        // the remainder from userdata's free space. The math is the same
        // for a freshly created PV, which also loses one extent.
        uint64_t sys_mb = GetSize(p.src) / kExtentSizeBytes;
        uint64_t rootfs_after = (sys_mb > 0) ? (sys_mb - 1) : 0;
        uint64_t extra = 0;
        if (required_rootfs_mb > rootfs_after)
            extra = required_rootfs_mb - rootfs_after;
        // Keep at least one extent of reserve when system is preserved: if
        // its filesystem turns out too full to shrink, the tail relocation
        // needs to grow the LV one extent past the partition size.
        if (!p.system_fresh && extra == 0) extra = 1;
        p.reserve_mb = extra;

        return p;
    }

    // Without a system partition there is no rootfs to extend, so LVM
    // serves no purpose on this device.
    Log("lvm-migrate: no system partition found; nothing to migrate");
    p.strategy = Strategy::ERROR;
    return p;
}

static bool RunFsMigrator(const std::string& device, const std::string& vg,
                           const std::string& lv, uint64_t reserve,
                           bool allow_tail_relocate = false) {
    // Remove any stale tail backup from an earlier failed attempt so a
    // backup's presence reliably signals this run's tail relocation.
    unlink(TailBackupPath(lv).c_str());

    std::vector<std::string> args = {
        "lvm-fs-migrator",
        "--device=" + device,
        "--vg-name=" + vg,
        "--lv-name=" + lv,
        "--reserve=" + std::to_string(reserve),
    };
    if (allow_tail_relocate)
        args.push_back("--allow-tail-relocate");

    bool ok = RunOrLog(args) == 0;
    if (!ok) unlink(TailBackupPath(lv).c_str());
    return ok;
}

// lvm-fs-migrator backs up the trailing 1MB of a filesystem it could not
// shrink; the migrated LV is then 1MB short of the filesystem it holds.
// Extend the LV past the original partition size (or to required_mb if
// that is larger), write the tail back at its original filesystem offset
// and grow the filesystem. A no-op when no backup exists.
static bool RestoreRootfsTail(const std::string& vg, const std::string& src,
                              uint64_t required_mb) {
    std::string tail = TailBackupPath("rootfs");
    if (access(tail.c_str(), F_OK) != 0) return true;

    uint64_t src_mb = GetSize(src) / kExtentSizeBytes;
    uint64_t target_mb = (required_mb > src_mb) ? required_mb : src_mb;
    Log("lvm-migrate: restoring unshrinkable rootfs tail"
        " (extending to " + std::to_string(target_mb) + " MB)");
    if (RunOrLog({"lvextend", "-L", std::to_string(target_mb) + "M",
                  vg + "/rootfs"}) != 0) {
        Log("lvm-migrate: cannot extend rootfs to restore its tail");
        return false;
    }

    std::string data;
    if (!android::base::ReadFileToString(tail, &data) ||
        data.size() != kExtentSizeBytes) {
        Log("lvm-migrate: rootfs tail backup is unreadable");
        return false;
    }
    std::string dev = "/dev/" + vg + "/rootfs";
    int fd = open(dev.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        Log("lvm-migrate: cannot open " + dev + " to restore the tail");
        return false;
    }
    bool ok = pwrite(fd, data.data(), data.size(),
                     (src_mb - 1) * kExtentSizeBytes) == (ssize_t)data.size();
    fsync(fd);
    close(fd);
    if (!ok) {
        Log("lvm-migrate: writing the rootfs tail back failed");
        return false;
    }
    unlink(tail.c_str());

    RunOrLog({"resize2fs", "-f", dev});
    return true;
}

// Size an LV would have right after in-place migration of the given
// partition: the raw partition minus one extent of LVM metadata, rounded
// down to a whole extent. Used to keep dry-run size decisions meaningful
// when the LV does not actually exist yet.
static uint64_t PredictedLvBytes(const std::string& device, uint64_t reserve_mb) {
    uint64_t size = GetSize(device);
    uint64_t reserved = (reserve_mb + 1) * kExtentSizeBytes;
    if (size <= reserved) return 0;
    return ((size - reserved) / kExtentSizeBytes) * kExtentSizeBytes;
}

// Set the completion property init waits on. Skipped in dry-run so a manual
// run on a live recovery does not fire triggers watching this property.
static void SetDoneProperty() {
    if (g_dry_run) {
        Log("lvm-migrate: [dry-run] would set halium.lvm_migration.done=1");
        return;
    }
    android::base::SetProperty("halium.lvm_migration.done", "1");
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--dry-run" || arg == "-n") {
            g_dry_run = true;
        } else if (arg == "--check" || arg == "-c") {
            g_check_only = true;
            g_dry_run = true;
        } else if (arg == "--help") {
            std::cout << "Usage: lvm-migrate [--dry-run|-n] [--check|-c]\n"
                         "  --dry-run  inspect state and log the migration plan"
                         " without modifying anything\n"
                         "  --check    like --dry-run, but exit 0 if nothing to"
                         " do, 2 if migration work is pending, 1 on error\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    g_kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

    // When run from recovery, its control socket leaks into every LVM
    // invocation (including lvm-fs-migrator's) and LVM prints a warning
    // per call; the fd is harmless, silence the noise for the whole tree.
    setenv("LVM_SUPPRESS_FD_WARNINGS", "1", 1);

    if (g_dry_run)
        Log("lvm-migrate: DRY RUN - no changes will be made");

    std::string use_lvm = android::base::GetProperty("ro.systemimage.use_lvm", "");
    if (use_lvm != "true" && use_lvm != "1") {
        Log("lvm-migrate: not requested, skipping");
        SetDoneProperty();
        return 0;
    }

    uint64_t required_mb = 0;
    {
        std::string v = android::base::GetProperty(
                "ro.systemimage.system_partition_size", "");
        if (!v.empty()) required_mb = strtoull(v.c_str(), nullptr, 10);
    }
    if (required_mb == 0) {
        Log("lvm-migrate: no target size set, nothing to do");
        SetDoneProperty();
        return 0;
    }

    static const char* kVg = "ubports";
    Layout layout = DetectLayout();
    Log("lvm-migrate: layout: has_super=" + std::to_string(layout.has_super)
        + " is_ab=" + std::to_string(layout.is_ab)
        + " slot=" + (layout.active_slot.empty() ? "none" : layout.active_slot)
        + " userdata=" + (layout.userdata.empty() ? "none" : layout.userdata)
        + " userdata_fs=" + ContentName(layout.userdata_content)
        + (layout.system_single.empty()
               ? std::string()
               : std::string(" system_fs=") + ContentName(layout.system_content))
        + " required_rootfs=" + std::to_string(required_mb) + " MB");
    Plan plan = MakePlan(layout, kVg, required_mb);

    int rc = 0;

    switch (plan.strategy) {
    case Strategy::DONE:
        Log("lvm-migrate: target state already reached");
        break;

    case Strategy::AB_SLOT_MERGE: {
        Log("lvm-migrate: migrating active slot " + plan.src
             + ", extending over spare " + plan.spare);
        if (!RunFsMigrator(plan.src, kVg, "rootfs", 0,
                           /*allow_tail_relocate=*/true)) {
            Log("lvm-migrate: system migration failed,"
                " leaving system partitions untouched");
            rc = 1;
            break;
        }
        if (RunOrLog({"pvcreate", "-y", Resolve(plan.spare)}) != 0 ||
            RunOrLog({"lvm", "vgextend", kVg, Resolve(plan.spare)}) != 0) {
            Log("lvm-migrate: extending VG onto the spare slot failed");
            rc = 1;
            break;
        }
        if (!RestoreRootfsTail(kVg, plan.src, required_mb)) {
            rc = 1;
            break;
        }
        // Extend rootfs up to the required size from spare slot free space.
        uint64_t lv_bytes = g_dry_run ? PredictedLvBytes(plan.src, 0)
                                      : GetLvSize(kVg, "rootfs");
        uint64_t required = required_mb * kExtentSizeBytes;
        if (lv_bytes > 0 && lv_bytes < required) {
            RunOrLog({"lvextend", "-L",
                    std::to_string(required_mb) + "M",
                    std::string(kVg) + "/rootfs"});
            RunOrLog({"resize2fs", "-f", "/dev/" + std::string(kVg) + "/rootfs"});
        }
        break;
    }

    case Strategy::NONAB_USERDATA_AND_SYSTEM: {
        Log("lvm-migrate: migrating userdata + system"
            " (reserve=" + std::to_string(plan.reserve_mb) + " MB"
            ", userdata=" + (plan.userdata_fresh ? "fresh" : "preserve") +
            ", system=" + (plan.system_fresh ? "fresh" : "preserve") + ")");
        std::string tmp = std::string(kVg) + "_data";

        if (plan.deactivate_first) RunOrLog({"vgchange", "-an", kVg});
        if (plan.wipe_system && !WipeStart(plan.src)) {
            Log("lvm-migrate: failed to wipe stale LVM label on " + plan.src);
            rc = 1;
            break;
        }

        if (plan.userdata_fresh && plan.system_fresh) {
            // Nothing to preserve: build the final VG directly. rootfs is
            // left without a filesystem; the installer formats it
            // ("format system" runs mkfs.ext4 on /dev/ubports/rootfs).
            if (RunOrLog({"pvcreate", "-y", Resolve(layout.userdata)}) != 0 ||
                RunOrLog({"pvcreate", "-y", Resolve(plan.src)}) != 0 ||
                RunOrLog({"lvm", "vgcreate", "-s", "1m", kVg,
                          Resolve(layout.userdata), Resolve(plan.src)}) != 0 ||
                RunOrLog({"lvm", "lvcreate", "-y",
                          "-L", std::to_string(required_mb) + "M",
                          "-n", "rootfs", kVg}) != 0 ||
                RunOrLog({"lvm", "lvcreate", "-y", "-l", "100%FREE",
                          "-n", "userdata", kVg}) != 0 ||
                RunOrLog({"mkfs.ext4",
                          "/dev/" + std::string(kVg) + "/userdata"}) != 0) {
                Log("lvm-migrate: fresh LVM setup failed");
                rc = 1;
            }
            break;
        }

        if (plan.system_fresh) {
            // Preserve userdata, recreate rootfs inside the same VG.
            if (!RunFsMigrator(layout.userdata, tmp, "userdata",
                               plan.reserve_mb)) {
                Log("lvm-migrate: userdata migration failed (resize2fs could"
                    " not free " + std::to_string(plan.reserve_mb + 1)
                    + " MB)");
                rc = 1;
                break;
            }
            if (RunOrLog({"pvcreate", "-y", Resolve(plan.src)}) != 0 ||
                RunOrLog({"lvm", "vgextend", tmp, Resolve(plan.src)}) != 0 ||
                RunOrLog({"lvm", "lvcreate", "-y",
                          "-L", std::to_string(required_mb) + "M",
                          "-n", "rootfs", tmp}) != 0) {
                Log("lvm-migrate: fresh rootfs creation failed");
                rc = 1;
                break;
            }
            RunOrLog({"lvm", "vgrename", tmp, kVg});
            break;
        }

        if (plan.userdata_fresh) {
            // Preserve system, recreate userdata: migrate system first,
            // extend the VG onto userdata, grow rootfs, rest is userdata.
            std::string sys_vg = std::string(kVg) + "_sys";
            if (!RunFsMigrator(plan.src, sys_vg, "rootfs", 0,
                               /*allow_tail_relocate=*/true)) {
                Log("lvm-migrate: system migration failed");
                rc = 1;
                break;
            }
            if (RunOrLog({"pvcreate", "-y", Resolve(layout.userdata)}) != 0 ||
                RunOrLog({"lvm", "vgextend", sys_vg,
                          Resolve(layout.userdata)}) != 0) {
                Log("lvm-migrate: extending VG onto userdata failed");
                rc = 1;
                break;
            }
            if (!RestoreRootfsTail(sys_vg, plan.src, required_mb)) {
                rc = 1;
                break;
            }
            uint64_t lv_bytes = g_dry_run ? PredictedLvBytes(plan.src, 0)
                                          : GetLvSize(sys_vg, "rootfs");
            if (lv_bytes < required_mb * kExtentSizeBytes) {
                RunOrLog({"lvextend", "-L",
                        std::to_string(required_mb) + "M",
                        sys_vg + "/rootfs"});
                RunOrLog({"resize2fs", "-f", "/dev/" + sys_vg + "/rootfs"});
            }
            if (RunOrLog({"lvm", "lvcreate", "-y", "-l", "100%FREE",
                          "-n", "userdata", sys_vg}) != 0 ||
                RunOrLog({"mkfs.ext4",
                          "/dev/" + sys_vg + "/userdata"}) != 0) {
                Log("lvm-migrate: fresh userdata creation failed");
                rc = 1;
                break;
            }
            RunOrLog({"lvm", "vgrename", sys_vg, kVg});
            break;
        }

        if (!RunFsMigrator(layout.userdata, tmp, "userdata", plan.reserve_mb)) {
            Log("lvm-migrate: userdata migration failed (resize2fs could not"
                " free " + std::to_string(plan.reserve_mb + 1) + " MB)");
            rc = 1;
            break;
        }
        std::string sys_vg = std::string(kVg) + "_sys";
        if (RunFsMigrator(plan.src, sys_vg, "rootfs", 0,
                          /*allow_tail_relocate=*/true)) {
            // vgmerge requires both source and destination VGs to be inactive.
            RunOrLog({"vgchange", "-an", tmp});
            RunOrLog({"vgchange", "-an", sys_vg});
            if (RunOrLog({"lvm", "vgmerge", tmp, sys_vg}) != 0) {
                Log("lvm-migrate: vgmerge failed");
                rc = 1;
                break;
            }
            RunOrLog({"vgchange", "-ay", tmp});
        } else {
            Log("lvm-migrate: system preservation failed,"
                 " leaving " + plan.src + " untouched");
        }
        RunOrLog({"lvm", "vgrename", tmp, kVg});

        if (!RestoreRootfsTail(kVg, plan.src, required_mb)) {
            rc = 1;
            break;
        }

        // Extend rootfs up to the required size from VG free space.
        uint64_t lv_bytes = g_dry_run ? PredictedLvBytes(plan.src, 0)
                                      : GetLvSize(kVg, "rootfs");
        uint64_t required = required_mb * kExtentSizeBytes;
        if (lv_bytes > 0 && lv_bytes < required) {
            RunOrLog({"lvextend", "-L",
                    std::to_string(required_mb) + "M",
                    std::string(kVg) + "/rootfs"});
            RunOrLog({"resize2fs", "-f", "/dev/" + std::string(kVg) + "/rootfs"});
        }
        break;
    }

    case Strategy::NONAB_GROW_ROOTFS: {
        std::string rootfs_dev = "/dev/" + std::string(kVg) + "/rootfs";
        std::string ud_dev = "/dev/" + std::string(kVg) + "/userdata";
        uint64_t rootfs_mb = GetLvSize(kVg, "rootfs") / kExtentSizeBytes;
        uint64_t free_mb = GetVgFree(kVg) / kExtentSizeBytes;
        Log("lvm-migrate: growing rootfs from " + std::to_string(rootfs_mb)
            + " MB to " + std::to_string(required_mb) + " MB ("
            + std::to_string(free_mb) + " MB free in VG)");

        if (rootfs_mb + free_mb < required_mb) {
            // Take the missing space back from the userdata LV. The
            // filesystem is shrunk first with the exact same size the LV
            // is reduced to; a mismatch here destroys user data, so both
            // use the same explicit MB value.
            uint64_t needed_mb = required_mb - rootfs_mb - free_mb;
            uint64_t ud_mb = GetLvSize(kVg, "userdata") / kExtentSizeBytes;
            if (ud_mb <= needed_mb) {
                Log("lvm-migrate: userdata is too small to give up "
                    + std::to_string(needed_mb) + " MB");
                rc = 1;
                break;
            }
            uint64_t new_ud_mb = ud_mb - needed_mb;
            Log("lvm-migrate: shrinking userdata from " + std::to_string(ud_mb)
                + " MB to " + std::to_string(new_ud_mb) + " MB");
            int fsck = RunOrLog({"e2fsck", "-f", "-y", ud_dev});
            if (fsck != 0 && fsck != 1) {
                Log("lvm-migrate: userdata filesystem check failed");
                rc = 1;
                break;
            }
            // resize2fs refuses upfront when the data does not fit.
            if (RunOrLog({"resize2fs", "-f", ud_dev,
                          std::to_string(new_ud_mb) + "M"}) != 0) {
                Log("lvm-migrate: userdata cannot shrink by "
                    + std::to_string(needed_mb) + " MB; free up space"
                      " and retry");
                rc = 1;
                break;
            }
            if (RunOrLog({"lvm", "lvreduce", "-f", "-L",
                          std::to_string(new_ud_mb) + "M",
                          std::string(kVg) + "/userdata"}) != 0) {
                Log("lvm-migrate: lvreduce failed");
                rc = 1;
                break;
            }
        }

        if (RunOrLog({"lvextend", "-L", std::to_string(required_mb) + "M",
                      std::string(kVg) + "/rootfs"}) != 0) {
            Log("lvm-migrate: extending rootfs failed");
            rc = 1;
            break;
        }
        int fsck = RunOrLog({"e2fsck", "-f", "-y", rootfs_dev});
        if (fsck == 0 || fsck == 1) {
            if (RunOrLog({"resize2fs", "-f", rootfs_dev}) != 0) {
                Log("lvm-migrate: growing the rootfs filesystem failed");
                rc = 1;
            }
        } else {
            // No valid filesystem yet (e.g. rootfs was never formatted);
            // the installer's "format system" step creates it at LV size.
            Log("lvm-migrate: no valid filesystem on rootfs, skipping"
                " filesystem resize");
        }
        break;
    }

    case Strategy::NONAB_CREATE_ROOTFS: {
        // Bring the system partition into the VG unless an interrupted run
        // already did, then create rootfs. No mkfs: the installer's
        // "format system" step creates the filesystem.
        std::string sys = Resolve(plan.src);
        std::string member_vg = android::base::Trim(
                RunCmdOutput({"pvs", "--noheadings", "-o", "vg_name", sys}));
        if (member_vg != kVg) {
            if (plan.wipe_system && !WipeStart(plan.src)) {
                Log("lvm-migrate: failed to wipe stale LVM label on "
                    + plan.src);
                rc = 1;
                break;
            }
            if (RunOrLog({"pvcreate", "-y", sys}) != 0 ||
                RunOrLog({"lvm", "vgextend", kVg, sys}) != 0) {
                Log("lvm-migrate: extending VG onto system failed");
                rc = 1;
                break;
            }
        }
        if (RunOrLog({"lvm", "lvcreate", "-y",
                      "-L", std::to_string(required_mb) + "M",
                      "-n", "rootfs", kVg}) != 0) {
            Log("lvm-migrate: rootfs creation failed");
            rc = 1;
        }
        break;
    }

    case Strategy::ERROR:
        Log("lvm-migrate: cannot proceed due to errors above");
        rc = 1;
        break;
    }

    SetDoneProperty();

    if (g_check_only) {
        if (rc != 0) return 1;
        if (g_pending_mutations > 0) {
            Log("lvm-migrate: check: " + std::to_string(g_pending_mutations)
                + " pending action(s)");
            return 2;
        }
        Log("lvm-migrate: check: nothing to do");
        return 0;
    }

    return rc;
}
