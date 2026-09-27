#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#define MIN(a, b) std::min((a), (b))
#define MAX(a, b) std::max((a), (b))
#define BUILD_ASSERT static_assert
#define DT_CHOSEN(x) 0
#define DT_BUS(x) 0
#define DT_PROP(x, y) 32000000
#define DEVICE_DT_GET(x) (&test_device)
#define FIXED_PARTITION_ID(x) 0
struct device {};
inline device test_device;
inline bool device_is_ready(const device *) { return true; }
struct flash_area { const device *fa_dev; size_t fa_size; };
inline flash_area test_area{&test_device, 0xA00000};
inline std::vector<uint8_t> flash_bytes;
inline int flash_reads, fail_read;
inline int flash_area_open(int, const flash_area **area) {
    *area = &test_area;
    return 0;
}
inline void flash_area_close(const flash_area *) {}
inline int flash_area_read(const flash_area *, size_t offset, void *data, size_t size) {
    ++flash_reads;
    if (fail_read && flash_reads == fail_read) return -1;
    assert(offset >= 0x1000 && offset - 0x1000 + size <= flash_bytes.size());
    memcpy(data, flash_bytes.data() + offset - 0x1000, size);
    return 0;
}
struct lv_color_t { uint16_t full; };
inline lv_color_t lv_color_make(int r, int g, int b) {
    return {static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))};
}
struct display_buffer_descriptor { size_t buf_size; uint16_t width, height, pitch; };
inline uint16_t lcd[320 * 172];
inline int writes;
inline int display_write(const device *, uint16_t x, uint16_t y,
                         const display_buffer_descriptor *d, const void *p) {
    assert(x + d->width <= 320 && y + d->height <= 172);
    assert(d->buf_size >= size_t(d->pitch) * d->height * 2);
    const auto *pixels = static_cast<const lv_color_t *>(p);
    for (int row = 0; row < d->height; ++row)
        for (int col = 0; col < d->width; ++col)
            lcd[(y + row) * 320 + x + col] = pixels[row * d->pitch + col].full;
    ++writes;
    return 0;
}
