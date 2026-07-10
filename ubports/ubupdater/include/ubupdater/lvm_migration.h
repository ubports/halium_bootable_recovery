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
#ifndef _UBUPDATER_LVM_MIGRATION_H_
#define _UBUPDATER_LVM_MIGRATION_H_

class RecoveryUI;

// Migration log written to tmpfs while /cache is unmounted; copied to
// Paths::Get().lvm_migrate_log_file() once the cache mount is back.
static constexpr const char* TEMPORARY_LVM_MIGRATE_LOG_FILE =
    "/tmp/lvm-migrate.log";

// Runs a pending LVM storage migration (lvm-migrate) with the install UI
// showing, unmounting /cache and /data around it and remounting via
// setup-fake-cache afterwards. Returns true when boot may proceed normally
// (nothing to do, or migration and remount succeeded); false on failure,
// in which case the error UI has been shown and the automatic Ubuntu OTA
// path is blocked.
bool MaybeRunLvmMigration(RecoveryUI* ui);

// True if the migration failed this boot; do_ubuntu_update refuses to run.
bool LvmMigrationBlockedOta();

#endif  // _UBUPDATER_LVM_MIGRATION_H_
