#pragma once
/*
 * mqtt_ha.h — MQTT + Home Assistant auto-discovery declarations
 */

#include "config.h"

void setupMQTT(const DeviceConfig& cfg, int pinButton, int pinLed);
void loopNormal();
