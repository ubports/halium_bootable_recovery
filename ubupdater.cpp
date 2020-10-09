/*
 * Copyright (C) 2016 The UBports Project
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
 *
 * Authored by: Marius Gripsgard <mariogrip@ubports.com>
 */
#include <unistd.h>

#include "ui.h"
#include "install.h"

extern "C" {
#include "libcrecovery/common.h"
}

static const char *UBUNTU_COMMAND_FILE = "/cache/recovery/ubuntu_command";
static const char *UBUNTU_UPDATE_SCRIPT = "/sbin/system-image-upgrader";
//static const char *UBUNTU_DEBUG_FILE = "/cache/recovery/ubuntu_debug";

//TODO: make and show "error screen" if ubpdate fails

int do_ubuntu_update(RecoveryUI *ui){
    ui->ShowText(true);
    ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
    ui->SetProgressType(RecoveryUI::INDETERMINATE);
    char tmp[PATH_MAX];
    sprintf(tmp, "%s %s &> /cache/ubuntu_updater.log", UBUNTU_UPDATE_SCRIPT, UBUNTU_COMMAND_FILE );
    /*if (access(UBUNTU_DEBUG_FILE, F_OK) != -1 )
        sprintf(tmp, "%s %s &> /cache/ubuntu_updater.log", UBUNTU_UPDATE_SCRIPT, UBUNTU_COMMAND_FILE );
    else
        sprintf(tmp, "%s %s", UBUNTU_UPDATE_SCRIPT, UBUNTU_COMMAND_FILE );*/
    if (__system(tmp) == 0) {
        ui->ShowText(false);
        return INSTALL_SUCCESS;
    }
    ui->SetProgressType(RecoveryUI::EMPTY);
    return INSTALL_ERROR;
}

int do_test_update(RecoveryUI *ui){
    ui->ShowText(true);
    ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
    ui->SetProgressType(RecoveryUI::INDETERMINATE);
    char tmp[PATH_MAX];
    sprintf(tmp, "%s %s", UBUNTU_UPDATE_SCRIPT, UBUNTU_COMMAND_FILE );
    sleep(20);
    return INSTALL_ERROR;
}
