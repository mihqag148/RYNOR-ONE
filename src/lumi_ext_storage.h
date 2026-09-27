/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUMI_EXT_GIF_BYTES      0xA00000U
#define LUMI_EXT_ASSET_BYTES    0x200000U
#define LUMI_EXT_RESERVE_BYTES  0x400000U
#define LUMI_EXT_ASSET_DATA_OFFSET 0x1000U
#define LUMI_EXT_ASSET_MAX_BYTES \
    (LUMI_EXT_ASSET_BYTES - LUMI_EXT_ASSET_DATA_OFFSET)

bool lumi_ext_storage_ready(void);

bool lumi_ext_asset_begin(size_t total_bytes);
bool lumi_ext_asset_chunk(
    uint32_t offset,
    const uint8_t *data,
    size_t len);
bool lumi_ext_asset_end(void);
void lumi_ext_asset_clear(void);
bool lumi_ext_asset_valid(void);
uint32_t lumi_ext_asset_size(void);
