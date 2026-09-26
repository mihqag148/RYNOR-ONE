/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

uint8_t lumi_battery_percent(void);
uint16_t lumi_battery_millivolts(void);
bool lumi_battery_ready(void);
