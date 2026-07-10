/*
 * Copyright (C) 2026 The UBports Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ubupdater/lvm_migration.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <otautil/paths.h>
#include <otautil/sysutil.h>
#include <recovery_ui/ui.h>

static const char* LVM_MIGRATE_BIN = "/system/bin/lvm-migrate";
static const char* SETUP_FAKE_CACHE_BIN = "/system/bin/setup-fake-cache";

// Exit codes of lvm-migrate --check.
static constexpr int kCheckNothingToDo = 0;
static constexpr int kCheckWorkPending = 2;

static bool s_ota_blocked = false;

bool LvmMigrationBlockedOta() {
    return s_ota_blocked;
}

// Run a command with stdout+stderr appended to the tmpfs migration log.
// Returns the exit code, or -1 if the child could not be run.
static int RunLogged(const std::vector<std::string>& args) {
    LOG(INFO) << "lvm-migration: running " << android::base::Join(args, ' ');

    int log_fd = open(TEMPORARY_LVM_MIGRATE_LOG_FILE,
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    pid_t pid = fork();
    if (pid == 0) {
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
        }
        auto argv = StringVectorToNullTerminatedArray(args);
        execv(argv[0], argv.data());
        _exit(127);
    }
    if (log_fd >= 0) close(log_fd);
    if (pid < 0) {
        PLOG(ERROR) << "lvm-migration: fork failed";
        return -1;
    }

    int status;
    if (TEMP_FAILURE_RETRY(waitpid(pid, &status, 0)) != pid) {
        PLOG(ERROR) << "lvm-migration: waitpid failed";
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Mount state from /proc/self/mounts; kUnknown when the table itself could
// not be read, so each caller can fail in the safe direction.
enum class MountState { kMounted, kUnmounted, kUnknown };

static MountState GetMountState(const std::string& mountpoint) {
    std::string mounts;
    // Not /proc/mounts: that is a symlink, and ReadFileToString refuses
    // symlinks by default (O_NOFOLLOW), which would report everything as
    // unmounted.
    if (!android::base::ReadFileToString("/proc/self/mounts", &mounts))
        return MountState::kUnknown;
    for (const auto& line : android::base::Split(mounts, "\n")) {
        auto fields = android::base::Split(line, " ");
        if (fields.size() >= 2 && fields[1] == mountpoint)
            return MountState::kMounted;
    }
    return MountState::kUnmounted;
}

// Append a diagnostic line to the migration log so unmount behavior shows
// up alongside the migrator output when triaging failures.
static void LogToFile(const std::string& line) {
    int fd = open(TEMPORARY_LVM_MIGRATE_LOG_FILE,
                  O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    std::string s = line + "\n";
    ssize_t n = write(fd, s.c_str(), s.size());
    (void)n;
    close(fd);
}

// Unmount a path, retrying on failure. Success is only ever decided by
// /proc/mounts, not by the umount() return value: mounts can be stacked
// (e.g. an fstab cache volume mounted over the fake-cache bind, where one
// successful umount still leaves a mount), and a spurious errno must not
// let migration run over a live filesystem. Never falls back to a lazy
// unmount: a detached-but-live filesystem while lvm-fs-migrator rewrites
// the raw device would corrupt it.
static bool UmountRetry(const char* mountpoint) {
    for (int i = 0; i < 10; i++) {
        // Only a confirmed-unmounted state counts as success; an unreadable
        // mount table must not let migration run over a live filesystem.
        if (GetMountState(mountpoint) == MountState::kUnmounted) return true;
        if (umount(mountpoint) != 0) {
            PLOG(WARNING) << "lvm-migration: umount " << mountpoint
                          << " failed";
            LogToFile(std::string("lvm-migration: umount ") + mountpoint +
                      " failed: " + strerror(errno) + " (attempt " +
                      std::to_string(i + 1) + ")");
            sync();
            usleep(500000);
        }
    }
    if (GetMountState(mountpoint) != MountState::kUnmounted) {
        LogToFile(std::string("lvm-migration: ") + mountpoint +
                  " still mounted after retries, aborting migration");
        return false;
    }
    return true;
}

// Record the /data and /cache mount entries and the /etc/mtab state in the
// migration log; what the migrator's fs tools will consider "mounted"
// depends on both.
static void LogMountState(const char* when) {
    std::string mounts;
    android::base::ReadFileToString("/proc/self/mounts", &mounts);
    std::string state = std::string("lvm-migration: mount state ") + when + ":";
    for (const auto& line : android::base::Split(mounts, "\n")) {
        if (line.find(" /data ") != std::string::npos ||
            line.find(" /cache ") != std::string::npos) {
            state += "\n  " + line;
        }
    }
    struct stat st;
    if (lstat("/etc/mtab", &st) != 0) {
        state += "\n  /etc/mtab: missing";
    } else if (S_ISLNK(st.st_mode)) {
        state += "\n  /etc/mtab: symlink";
    } else {
        state += "\n  /etc/mtab: REGULAR FILE (stale entries possible)";
    }
    LogToFile(state);
}

static void ShowMigrationError(RecoveryUI* ui, const char* log_path) {
    ui->ShowText(true);
    ui->Print("Error: LVM storage migration failed.\n");
    ui->Print("Please go to Advanced -> View recovery logs -> %s\n", log_path);
    ui->SetBackground(RecoveryUI::ERROR);
    ui->SetProgressType(RecoveryUI::EMPTY);
}

bool MaybeRunLvmMigration(RecoveryUI* ui) {
    if (access(LVM_MIGRATE_BIN, X_OK) != 0) return true;

    int check = RunLogged({LVM_MIGRATE_BIN, "--check"});
    if (check == kCheckNothingToDo) return true;
    if (check != kCheckWorkPending) {
        LOG(ERROR) << "lvm-migration: --check failed with code " << check;
        s_ota_blocked = true;
        ShowMigrationError(ui, TEMPORARY_LVM_MIGRATE_LOG_FILE);
        return false;
    }

    LOG(INFO) << "lvm-migration: migration pending, showing install UI";
    ui->ShowText(false);
    // Won't be visible until it finishes or fails, but just in case
    ui->Print("Migrating storage to LVM, do not power off...\n");
    ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
    ui->SetProgressType(RecoveryUI::INDETERMINATE);
    ui->SetEnableReboot(false);

    LogMountState("before unmount");

    // The bind mount must go first: while it exists, /data stays busy.
    sync();
    bool ok = UmountRetry("/cache") && UmountRetry("/data");
    LogMountState("after unmount");
    if (!ok) {
        LOG(ERROR) << "lvm-migration: could not unmount /cache and /data";
    } else {
        ok = RunLogged({LVM_MIGRATE_BIN}) == 0;
    }

    // Remount best-effort even after a failure so logs and menus keep
    // working; setup-fake-cache prefers the migrated LV when it exists.
    int remount_rc = RunLogged({SETUP_FAKE_CACHE_BIN});
    bool remounted = remount_rc == 0 &&
                     GetMountState("/data") == MountState::kMounted &&
                     GetMountState("/cache") == MountState::kMounted;

    // Best-effort copy of the log to /cache so it is reachable from the
    // view-logs menu even when the remount check failed.
    std::string cache_log = Paths::Get().lvm_migrate_log_file();
    std::string contents;
    if (android::base::ReadFileToString(TEMPORARY_LVM_MIGRATE_LOG_FILE,
                                        &contents)) {
        android::base::WriteStringToFile(contents, cache_log);
    }

    ui->SetEnableReboot(true);
    if (ok && remounted) {
        LOG(INFO) << "lvm-migration: migration finished successfully";
        ui->SetProgressType(RecoveryUI::EMPTY);
        ui->SetBackground(RecoveryUI::NONE);
        return true;
    }

    s_ota_blocked = true;
    ShowMigrationError(
        ui, remounted ? cache_log.c_str() : TEMPORARY_LVM_MIGRATE_LOG_FILE);
    return false;
}
