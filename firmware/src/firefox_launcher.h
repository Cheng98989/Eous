#pragma once

// Opens Firefox on the PC connected via USB.
// The ESP32-S3 shows up as a USB keyboard and types the commands.

void launcherInit();    // call once in setup()
void launchFirefox();   // call whenever you want to open Firefox