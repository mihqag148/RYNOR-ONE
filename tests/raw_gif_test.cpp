#include "../src/lumi_raw_gif.cpp"
#include <cstdio>

extern "C" void lumi_diag_report(char, const char *, ...) {}

static void test_flash_cache() {
    flash_bytes.resize(3107);
    for (size_t i = 0; i < flash_bytes.size(); ++i) flash_bytes[i] = i * 17;
    g_data_size = flash_bytes.size();
    int32_t size;
    assert(gif_open("", &size));
    GIFFILE file{};
    file.iSize = size;
    uint8_t byte;
    for (int i = 0; i < size; ++i) {
        assert(gif_read(&file, &byte, 1) == 1 && byte == flash_bytes[i]);
    }
    assert(flash_reads == 4); // 3107 small reads become four NOR reads.
    assert(gif_read(&file, &byte, 1) == 0);
    uint8_t block[2200];
    gif_seek(&file, 900);
    assert(gif_read(&file, block, sizeof(block)) == sizeof(block));
    assert(memcmp(block, flash_bytes.data() + 900, sizeof(block)) == 0);
    gif_seek(&file, 1030);
    assert(gif_read(&file, block, 500) == 500);
    assert(memcmp(block, flash_bytes.data() + 1030, 500) == 0);
    gif_close(nullptr);
    flash_bytes[1030] ^= 0xFF; // Reopening after replacement must invalidate cache.
    assert(gif_open("", &size));
    gif_seek(&file, 1030);
    assert(gif_read(&file, &byte, 1) == 1 && byte == flash_bytes[1030]);
    gif_close(nullptr);
    assert(gif_open("", &size));
    gif_seek(&file, 0);
    fail_read = flash_reads + 2;
    assert(gif_read(&file, block, sizeof(block)) == 1024);
    assert(file.iPos == 1024);
    fail_read = 0;
    assert(gif_read(&file, block, sizeof(block)) == size - 1024);
    assert(memcmp(block, flash_bytes.data() + 1024, size - 1024) == 0);
    gif_close(nullptr);
}

static void test_render_rows(bool interlaced) {
    g_source_width = 320;
    g_source_height = 172;
    g_scale_mode = 0;
    compute_transform();
    uint16_t palette[256];
    uint8_t pixels[320];
    for (int i = 0; i < 256; ++i) palette[i] = i * 251;
    GIFDRAW draw{};
    draw.iWidth = 320;
    draw.iHeight = 172;
    draw.pPalette = palette;
    draw.pPixels = pixels;
    auto row = [&](int y) {
        draw.y = y;
        for (int x = 0; x < 320; ++x) pixels[x] = (x + y) % 256;
        gif_draw(&draw);
    };
    if (interlaced) {
        for (int y = 0; y < 172; y += 8) row(y);
        for (int y = 4; y < 172; y += 8) row(y);
        for (int y = 2; y < 172; y += 4) row(y);
        for (int y = 1; y < 172; y += 2) row(y);
    } else {
        for (int y = 0; y < 172; ++y) row(y);
    }
    flush_stripe();
    for (int y = 0; y < 172; ++y)
        for (int x = 0; x < 320; ++x)
            assert(lcd[y * 320 + x] == palette[(x + y) % 256]);
    int before = writes;
    g_suppress_opaque_output = true;
    row(0);
    flush_stripe();
    assert(writes == before);
    // Transparent partial rows must not be dropped or overwrite retained pixels.
    draw.iX = 10;
    draw.iY = 2;
    draw.y = 0;
    draw.iWidth = 2;
    draw.iHeight = 1;
    draw.ucHasTransparency = 1;
    draw.ucTransparent = 0;
    pixels[0] = 0;
    pixels[1] = 200;
    uint16_t previous = lcd[2 * 320 + 10];
    gif_draw(&draw);
    assert(g_frame_forced_present);
    assert(lcd[2 * 320 + 10] == previous && lcd[2 * 320 + 11] == palette[200]);
    g_suppress_opaque_output = false;
}

int main() {
    test_flash_cache();
    test_render_rows(false);
    test_render_rows(true);
    puts("Raw GIF cache, native/interlaced output and transparency tests passed");
}
