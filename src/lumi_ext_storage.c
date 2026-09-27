/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include "lumi_diag.h"
#include "lumi_ext_storage.h"

#define ASSET_MAGIC 0x5453414CU /* "LAST" little-endian */
#define ASSET_VERSION 1U

struct lumi_asset_header {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t data_size;
    uint32_t checksum;
};

static const struct flash_area *asset_area;
static bool asset_checked;
static bool asset_valid;
static bool asset_uploading;
static uint32_t asset_size;
static uint32_t asset_expected;
static uint32_t asset_received;
static uint32_t asset_checksum;

static uint32_t asset_checksum_update(
    uint32_t hash,
    const uint8_t *data,
    size_t len) {

    /* FNV-1a: compact incremental integrity check with no extra subsystem. */
    for (size_t i = 0U; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }

    return hash;
}

static int open_area(
    int id,
    uint32_t expected_size,
    const struct flash_area **area_out) {

    const struct flash_area *area = NULL;
    int rc = flash_area_open(id, &area);
    if (rc != 0 || !area) {
        return rc != 0 ? rc : -ENODEV;
    }

    if (area->fa_size < expected_size) {
        flash_area_close(area);
        return -ENOSPC;
    }

    *area_out = area;
    return 0;
}

static int open_asset_once(void) {
    if (asset_area) {
        return 0;
    }

    int rc = open_area(
        FIXED_PARTITION_ID(lumi_assets_partition),
        LUMI_EXT_ASSET_BYTES,
        &asset_area);

    if (rc != 0) {
        asset_area = NULL;
        lumi_diag_report('E', "Asset flash open rc=%d", rc);
    }

    return rc;
}

bool lumi_ext_storage_ready(void) {
    const struct flash_area *gif = NULL;
    const struct flash_area *reserve = NULL;

    if (open_asset_once() != 0) {
        return false;
    }

    int gif_rc = open_area(
        FIXED_PARTITION_ID(lumi_saver_partition),
        LUMI_EXT_GIF_BYTES,
        &gif);

    int reserve_rc = open_area(
        FIXED_PARTITION_ID(lumi_reserve_partition),
        LUMI_EXT_RESERVE_BYTES,
        &reserve);

    if (gif) {
        flash_area_close(gif);
    }
    if (reserve) {
        flash_area_close(reserve);
    }

    return gif_rc == 0 && reserve_rc == 0;
}

static bool load_asset_header(void) {
    if (asset_checked) {
        return asset_valid;
    }

    asset_checked = true;
    asset_valid = false;
    asset_size = 0U;

    if (open_asset_once() != 0) {
        return false;
    }

    struct lumi_asset_header header = {0};
    int rc = flash_area_read(
        asset_area,
        0,
        &header,
        sizeof(header));

    if (rc != 0 ||
        header.magic != ASSET_MAGIC ||
        header.version != ASSET_VERSION ||
        header.data_size == 0U ||
        header.data_size > LUMI_EXT_ASSET_MAX_BYTES) {
        return false;
    }

    /* The upload path computes and stores a checksum before committing the
     * header. Do not rescan up to 2 MiB synchronously on every first status
     * query after boot; this keeps BLE/USB status reads responsive. A future
     * asset consumer can verify the stored checksum while streaming the pack.
     */
    asset_size = header.data_size;
    asset_valid = true;
    return true;
}

bool lumi_ext_asset_begin(size_t total_bytes) {
    if (total_bytes == 0U ||
        total_bytes > LUMI_EXT_ASSET_MAX_BYTES ||
        open_asset_once() != 0) {
        return false;
    }

    /* Erase the complete 2 MiB asset partition before a new pack. The
     * W25Q128 driver uses the largest supported erase blocks where possible.
     * A valid header is written only at ASSETEND, so interrupted uploads are
     * safely rejected on the next boot.
     */
    int rc = flash_area_erase(
        asset_area,
        0,
        LUMI_EXT_ASSET_BYTES);

    if (rc != 0) {
        lumi_diag_report('E', "Asset erase rc=%d", rc);
        return false;
    }

    asset_checked = true;
    asset_valid = false;
    asset_uploading = true;
    asset_size = 0U;
    asset_expected = (uint32_t)total_bytes;
    asset_received = 0U;
    asset_checksum = 2166136261U;

    return true;
}

bool lumi_ext_asset_chunk(
    uint32_t offset,
    const uint8_t *data,
    size_t len) {

    if (!asset_uploading ||
        !asset_area ||
        !data ||
        len == 0U ||
        offset != asset_received ||
        (uint64_t)offset + len > asset_expected) {
        return false;
    }

    int rc = flash_area_write(
        asset_area,
        LUMI_EXT_ASSET_DATA_OFFSET + offset,
        data,
        len);

    if (rc != 0) {
        lumi_diag_report(
            'E',
            "Asset write off=%u len=%u rc=%d",
            (unsigned int)offset,
            (unsigned int)len,
            rc);
        return false;
    }

    asset_checksum = asset_checksum_update(
        asset_checksum,
        data,
        len);
    asset_received += (uint32_t)len;
    return true;
}

bool lumi_ext_asset_end(void) {
    if (!asset_uploading ||
        !asset_area ||
        asset_expected == 0U ||
        asset_received != asset_expected) {
        return false;
    }

    struct lumi_asset_header header = {
        .magic = ASSET_MAGIC,
        .version = ASSET_VERSION,
        .reserved = 0U,
        .data_size = asset_expected,
        .checksum = asset_checksum,
    };

    int rc = flash_area_write(
        asset_area,
        0,
        &header,
        sizeof(header));

    if (rc != 0) {
        lumi_diag_report('E', "Asset header write rc=%d", rc);
        return false;
    }

    asset_uploading = false;
    asset_size = asset_expected;
    asset_valid = true;
    asset_checked = true;

    lumi_diag_report(
        'I',
        "Asset pack ready bytes=%u sum=%08x",
        (unsigned int)asset_size,
        (unsigned int)asset_checksum);

    return true;
}

void lumi_ext_asset_clear(void) {
    if (open_asset_once() == 0) {
        (void)flash_area_erase(
            asset_area,
            0,
            LUMI_EXT_ASSET_DATA_OFFSET);
    }

    asset_checked = true;
    asset_valid = false;
    asset_uploading = false;
    asset_size = 0U;
    asset_expected = 0U;
    asset_received = 0U;
    asset_checksum = 2166136261U;
}

bool lumi_ext_asset_valid(void) {
    return load_asset_header();
}

uint32_t lumi_ext_asset_size(void) {
    return load_asset_header() ? asset_size : 0U;
}
