#include "../src/lumi_raw_gif.cpp"
#include <cstdio>

extern "C" void lumi_diag_report(char, const char *, ...) {}

static void test_flash_cache() {
    flash_bytes.resize(10007);
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
    assert(flash_reads == 3); // 10007 single-byte reads become three 4 KiB NOR reads.
    assert(gif_read(&file, &byte, 1) == 0);
    uint8_t block[9000];
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
    assert(gif_read(&file, block, sizeof(block)) == 4096);
    assert(file.iPos == 4096);
    fail_read = 0;
    assert(gif_read(&file, block, sizeof(block)) == size - 4096);
    assert(memcmp(block, flash_bytes.data() + 4096, size - 4096) == 0);
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

static void test_rgb444_pack() {
    uint16_t palette[256] = {};
    uint8_t pixels[320] = {};

    /* AnimatedGIF RGB565_BE numeric values on little-endian nRF52840:
     * red=0x00F8, green=0xE007. Two pixels become RGB444 bytes F0 0F F0.
     */
    palette[1] = 0x00F8;
    palette[2] = 0xE007;
    pixels[0] = 1;
    pixels[1] = 2;

    g_fast_panel_mode = true;
    g_rgb444_rows = 0;
    g_rgb444_start_y = -1;

    append_rgb444_row(0, pixels, palette);

    assert(g_rgb444_rows == 1);
    assert(g_rgb444_start_y == 0);
    assert(g_rgb444_stripes[g_rgb444_fill_index][0] == 0xF0);
    assert(g_rgb444_stripes[g_rgb444_fill_index][1] == 0x00);
    assert(g_rgb444_stripes[g_rgb444_fill_index][2] == 0xF0);

    g_rgb444_rows = 0;
    g_rgb444_start_y = -1;
    g_fast_panel_mode = false;
}

static void test_complete_gif() {
    // A complete native-size GIF, with literal LZW codes and alternating
    // red/green pixels. Exercise the real decoder and render_due frame tail,
    // not only direct row callbacks. Clear codes keep the code width at 3.
    flash_bytes = {'G','I','F','8','9','a', 0x40,1, 172,0, 0x80,0,0,
                   255,0,0, 0,255,0,
                   0x21,0xF9,4,0,4,0,0,0,
                   0x2C,0,0,0,0,0x40,1,172,0,0,2};
    std::vector<uint8_t> compressed;
    unsigned bits = 0, count = 0;
    auto code = [&](unsigned value) {
        bits |= value << count;
        count += 3;
        while (count >= 8) {
            compressed.push_back(bits & 255);
            bits >>= 8;
            count -= 8;
        }
    };
    for (int i = 0; i < 320 * 172; ++i) { code(4); code(i % 2); }
    code(5);
    if (count) compressed.push_back(bits & 255);
    for (size_t i = 0; i < compressed.size(); i += 255) {
        size_t size = std::min(size_t(255), compressed.size() - i);
        flash_bytes.push_back(size);
        flash_bytes.insert(flash_bytes.end(), compressed.begin() + i,
                           compressed.begin() + i + size);
    }
    flash_bytes.push_back(0);
    flash_bytes.push_back(0x3B);
    lumi_raw_gif_configure(flash_bytes.size(), 0);
    writes = 0;
    assert(lumi_raw_gif_render_due(0));
    assert(writes == 44); // 22 clear stripes + 22 complete image stripes.
    for (int i = 0; i < 320 * 172; ++i)
        assert(lcd[i] == (i % 2 ? 0xE007 : 0x00F8)); // RGB565 big-endian
    int before = writes;
    assert(lumi_raw_gif_render_due(10));
    assert(writes == before); // Respect GIF's 40 ms frame delay.
    assert(lumi_raw_gif_render_due(40)); // Restart at EOF through cached seeks.
    assert(writes == before + 22);
    lumi_raw_gif_stop();
}

int main() {
    test_flash_cache();
    test_render_rows(false);
    test_render_rows(true);
    test_rgb444_pack();
    test_complete_gif();
    puts("Raw GIF cache, rendering, transparency and complete decode tests passed");
}
