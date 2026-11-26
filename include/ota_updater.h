#pragma once
#include <Arduino.h>

void checkAndPromptFirmwareUpdate();
void triggerCloudOTAFromWeb(const String &binUrl);
String fetchRemoteVersionJson();
