/*
 * Copyright (C) 2016-2021 The UBports Project
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

#include <install/install.h>
#include <otautil/roots.h>
#include <recovery_ui/ui.h>

static const char *UBUNTU_COMMAND_FILE = "/cache/recovery/ubuntu_command";
static const char *UBUNTU_UPDATE_SCRIPT = "/sbin/system-image-upgrader";

int setup_partitions(RecoveryUI *ui) {
    // Map logical partitions so there is a device node to put into fstab
    if (setup_install_mounts() != 0) {
        ui->Print("Failed to set up expected mounts for install; aborting\n");
        return INSTALL_ERROR;
    }

    // updating volume table including /etc/fstab, which is used by our script as
    // we are doing manual mounts where we only specify the path.
    load_volume_table();

    return INSTALL_SUCCESS;
}

void show_installation_error(RecoveryUI *ui, int result) {
    // Enable text to show errors to the user
    ui->ShowText(true);
    ui->Print("Error installing Ubuntu update, exit code: %d\n", result);
    ui->Print("Please go to Advanced -> View recovery logs -> /cache/ubuntu_updater.log\n");

    // TODO: Show error ui instead of text only?
    ui->SetProgressType(RecoveryUI::EMPTY);
}

int do_ubuntu_update(RecoveryUI *ui){
    // Disable text because otherwise the animation is not showing
    ui->ShowText(false);

    ui->Print("Setting up partitions...\n");
    int result = setup_partitions(ui);
    if (result != INSTALL_SUCCESS) {
        show_installation_error(ui, result);
        return INSTALL_ERROR;
    }

    ui->Print("Executing Ubuntu update script...\n");
    ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
    ui->SetProgressType(RecoveryUI::INDETERMINATE);

    char tmp[PATH_MAX];
    sprintf(tmp, "%s %s &> /cache/ubuntu_updater.log", UBUNTU_UPDATE_SCRIPT, UBUNTU_COMMAND_FILE);
    result = system(tmp);
    if (result != 0) {
        show_installation_error(ui, result);
        return INSTALL_ERROR;
    }

    ui->SetEnableReboot(true);
    ui->Print("\n");
    return INSTALL_SUCCESS;
}

int do_test_update(RecoveryUI *ui){
    // Disable text because otherwise the animation is not showing
    ui->ShowText(false);

    ui->Print("Executing Ubuntu update script...\n");
    ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
    ui->SetProgressType(RecoveryUI::INDETERMINATE);

    // Wait for 10 seconds to showcase the awesome install animation
    sleep(10);

    // Enable text, which stops the install animation
    ui->ShowText(true);
    ui->Print("... not really though, this is just a test.\n");

    char tmp[PATH_MAX];
    sprintf(tmp, "%s %s &> /cache/ubuntu_updater.log", UBUNTU_UPDATE_SCRIPT, UBUNTU_COMMAND_FILE);
    ui->Print("\nNormally this would call ->\n%s\n", tmp);

    ui->Print("\nCouting down from 5 instead.\n");
    ui->Print("5");
    sleep(1);
    ui->Print(" 4");
    sleep(1);
    ui->Print(" 3");
    sleep(1);
    ui->Print(" 2");
    sleep(1);
    ui->Print(" 1");
    sleep(1);
    ui->Print("\n");

    ui->Print("\nDone with counting, have a nice day!\n");

    return INSTALL_ERROR;
}
