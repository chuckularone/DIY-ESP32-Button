#pragma once
/*
 * portal.h — Captive portal declarations
 */

#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "config.h"

void setupPortal(WebServer& server, Preferences& prefs, DeviceConfig& cfg);
void loopPortal(WebServer& server, DNSServer& dns);
