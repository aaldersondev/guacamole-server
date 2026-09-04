/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/**
 * Offline benchmark for the guac_display rendering pipeline.
 *
 * This tool drives guac_display exactly as a protocol client (VNC, RDP, ...)
 * would, but with synthetic frames and with the resulting Guacamole
 * instruction stream discarded. It therefore measures the cost of everything
 * guacd does per frame -- dirty region detection, scroll/copy search,
 * operation combining, and image encoding -- without any network or remote
 * desktop server in the loop.
 *
 * Usage: guacbench [--width W] [--height H] [--frames N] [--image FILE.png]
 *                  [--scenario NAME] [--csv] [--out PREFIX] [--pace MS]
 *
 * Passing --out writes the Guacamole instruction stream produced for each
 * scenario to PREFIX.SCENARIO. Run under "taskset -c 0" so that a single
 * worker thread produces a deterministic ordering, and with --pace so that
 * both builds observe the same frame rate, this allows the output of two
 * builds to be compared byte for byte.
 */

#include "config.h"

#include "display-priv.h"

#include <guacamole/client.h>
#include <guacamole/display.h>
#include <guacamole/flag.h>
#include <guacamole/rect.h>
#include <guacamole/socket.h>
#include <guacamole/timestamp.h>

#include <cairo/cairo.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/**
 * The source image used to paint synthetic frames, as 32-bit ARGB/RGB pixels.
 */
static uint32_t* source = NULL;

/**
 * The width of the source image, in pixels.
 */
static int source_width = 0;

/**
 * The height of the source image, in pixels.
 */
static int source_height = 0;

/**
 * Returns the current wall clock time in microseconds.
 *
 * @return
 *     The current wall clock time, in microseconds.
 */
static uint64_t bench_now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * UINT64_C(1000000) + ts.tv_nsec / 1000;
}

/**
 * Returns the total CPU time consumed by this process (all threads, user and
 * system) in microseconds.
 *
 * @return
 *     The total CPU time consumed so far, in microseconds.
 */
static uint64_t bench_cpu() {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec * UINT64_C(1000000) + usage.ru_utime.tv_usec
         + usage.ru_stime.tv_sec * UINT64_C(1000000) + usage.ru_stime.tv_usec;
}

/**
 * Generates a synthetic source image resembling a desktop: a light background
 * with darker text-like runs, plus a photographic region of smooth gradients
 * and noise. Used when no real screenshot is supplied.
 *
 * @param width
 *     The width of the image to generate, in pixels.
 *
 * @param height
 *     The height of the image to generate, in pixels.
 */
static void bench_generate_source(int width, int height) {

    source_width = width;
    source_height = height;
    source = malloc((size_t) width * height * 4);

    uint32_t seed = 0x12345678;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {

            seed = seed * 1103515245 + 12345;
            uint32_t rnd = (seed >> 16) & 0xFF;

            uint32_t pixel;

            /* Lower right third is photographic: smooth gradients plus noise */
            if (x > width * 2 / 3 && y > height * 2 / 3) {
                int r = (x * 255 / width + rnd / 8) & 0xFF;
                int g = (y * 255 / height + rnd / 8) & 0xFF;
                int b = ((x + y) * 255 / (width + height) + rnd / 8) & 0xFF;
                pixel = 0xFF000000 | (r << 16) | (g << 8) | b;
            }

            /* Everything else is text-like: light background, dark glyph runs
             * that repeat along each line */
            else {
                int glyph = ((x / 3 + y / 17 * 7) % 11) < 4 && (y % 17) < 12;
                pixel = glyph ? 0xFF202020 : 0xFFF2F2F2;
                if (!glyph && (rnd & 0x3F) == 0)
                    pixel = 0xFFDDDDDD;
            }

            source[(size_t) y * width + x] = pixel;

        }
    }

}

/**
 * Loads the given PNG file as the source image for synthetic frames.
 *
 * @param path
 *     The path of the PNG file to load.
 *
 * @return
 *     Non-zero if the image was loaded successfully, zero otherwise.
 */
static int bench_load_source(const char* path) {

    cairo_surface_t* surface = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return 0;
    }

    source_width = cairo_image_surface_get_width(surface);
    source_height = cairo_image_surface_get_height(surface);

    int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    source = malloc((size_t) source_width * source_height * 4);
    for (int y = 0; y < source_height; y++)
        memcpy(source + (size_t) y * source_width, data + (size_t) y * stride,
                (size_t) source_width * 4);

    cairo_surface_destroy(surface);
    return 1;

}

/**
 * Copies a rectangle of the source image into the given layer buffer,
 * wrapping around the source image as necessary.
 *
 * @param buffer
 *     The destination image buffer.
 *
 * @param stride
 *     The number of bytes in each row of the destination image buffer.
 *
 * @param rect
 *     The destination rectangle to fill.
 *
 * @param offset_x
 *     The horizontal offset within the source image to read from.
 *
 * @param offset_y
 *     The vertical offset within the source image to read from.
 */
static void bench_blit(unsigned char* buffer, size_t stride,
        const guac_rect* rect, int offset_x, int offset_y) {

    for (int y = rect->top; y < rect->bottom; y++) {

        uint32_t* dst = (uint32_t*) (buffer + (size_t) y * stride) + rect->left;

        int src_y = (y + offset_y) % source_height;
        if (src_y < 0) src_y += source_height;

        const uint32_t* src_row = source + (size_t) src_y * source_width;

        for (int x = rect->left; x < rect->right; x++) {
            int src_x = (x + offset_x) % source_width;
            if (src_x < 0) src_x += source_width;
            *(dst++) = src_row[src_x];
        }

    }

}

/**
 * Waits until the display has no frame in progress, ensuring each benchmarked
 * frame is measured in full rather than being deferred and merged into the
 * next.
 *
 * @param display
 *     The display to wait on.
 */
static void bench_wait_idle(guac_display* display) {

    for (;;) {

        /* The render_state flag cannot be used on its own here: it still reads
         * as "no frame in progress" during the window between the frame being
         * queued and a worker thread picking it up. Whether work remains is
         * instead read from the operation queue itself, which is updated
         * before guac_display_end_frame() returns. */

        guac_fifo_lock(&display->ops);
        int busy = (display->ops.state.value & GUAC_FIFO_STATE_NONEMPTY)
                || display->active_workers
                || display->frame_deferred;
        guac_fifo_unlock(&display->ops);

        if (!busy)
            return;

        /* Wait without consuming CPU, which would otherwise be attributed to
         * this benchmark rather than to the encoding being measured */
        struct timespec interval = { .tv_sec = 0, .tv_nsec = 20000 };
        nanosleep(&interval, NULL);

    }

}

/**
 * Paints a single frame of the named scenario into the given layer.
 *
 * @param layer
 *     The layer to paint.
 *
 * @param scenario
 *     The name of the scenario being run.
 *
 * @param frame
 *     The ordinal number of the frame being painted.
 *
 * @param width
 *     The width of the layer, in pixels.
 *
 * @param height
 *     The height of the layer, in pixels.
 */
static void bench_paint(guac_display_layer* layer, const char* scenario,
        int frame, int width, int height) {

    guac_display_layer_raw_context* context = guac_display_layer_open_raw(layer);
    guac_rect dirty;

    /* A small caret sized region changing at a fixed location, as when typing
     * into an editor or terminal. Almost nothing changes, so this measures the
     * fixed per-frame overhead of the pipeline. */
    if (!strcmp(scenario, "typing")) {
        guac_rect_init(&dirty, 320, 400, 24, 40);
        bench_blit(context->buffer, context->stride, &dirty, frame * 24, 0);
    }

    /* A text region scrolling upward, as when scrolling a document or watching
     * output stream past in a terminal. Exercises the scroll/copy search. */
    else if (!strcmp(scenario, "scroll")) {
        guac_rect_init(&dirty, 0, 0, width, height);
        bench_blit(context->buffer, context->stride, &dirty, 0, frame * 16);
    }

    /* A medium window being dragged diagonally across the desktop. */
    else if (!strcmp(scenario, "window")) {
        int x = (frame * 7) % (width - 600);
        int y = (frame * 5) % (height - 400);
        guac_rect_init(&dirty, x, y, 600, 400);
        bench_blit(context->buffer, context->stride, &dirty, x, y);
    }

    /* A video sized region of photographic content changing every frame. */
    else if (!strcmp(scenario, "video")) {
        guac_rect_init(&dirty, 100, 100, 640, 360);
        bench_blit(context->buffer, context->stride, &dirty,
                source_width * 2 / 3 + frame * 3, source_height * 2 / 3 + frame);
    }

    /* The entire display changing every frame, as when playing full screen
     * video or switching virtual desktops. */
    else {
        guac_rect_init(&dirty, 0, 0, width, height);
        bench_blit(context->buffer, context->stride, &dirty, frame * 3, frame * 7);
    }

    guac_rect_extend(&context->dirty, &dirty);
    guac_display_layer_close_raw(layer, context);

}

int main(int argc, char** argv) {

    int width = 1600;
    int height = 900;
    int frames = 300;
    int csv = 0;
    const char* image = NULL;
    const char* only = NULL;
    const char* out = NULL;
    int pace = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--width") && i + 1 < argc) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) image = argv[++i];
        else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--pace") && i + 1 < argc) pace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--csv")) csv = 1;
        else {
            fprintf(stderr, "Usage: %s [--width W] [--height H] [--frames N] "
                    "[--image FILE.png] [--scenario NAME] [--csv]\n", argv[0]);
            return 1;
        }
    }

    if (image != NULL) {
        if (!bench_load_source(image)) {
            fprintf(stderr, "Unable to read image: %s\n", image);
            return 1;
        }
    }
    else
        bench_generate_source(width, height);

    const char* scenarios[] = { "typing", "scroll", "window", "video", "fullscreen" };
    int scenario_count = sizeof(scenarios) / sizeof(scenarios[0]);

    if (!csv)
        printf("%-12s %8s %10s %10s %10s %10s\n",
                "scenario", "frames", "wall(ms)", "cpu(ms)", "ms/frame", "cpu/frame");
    else
        printf("scenario,frames,wall_ms,cpu_ms,ms_per_frame,cpu_ms_per_frame\n");

    for (int s = 0; s < scenario_count; s++) {

        const char* scenario = scenarios[s];
        if (only != NULL && strcmp(only, scenario))
            continue;

        /* Each scenario gets a fresh client, display, and worker threads so
         * that no state carries over between measurements */
        guac_client* client = guac_client_alloc();

        int fd;
        if (out != NULL) {
            char path[4096];
            snprintf(path, sizeof(path), "%s.%s", out, scenario);
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        }
        else
            fd = open("/dev/null", O_WRONLY);

        client->socket = guac_socket_open(fd);

        guac_display* display = guac_display_alloc(client);
        guac_display_layer* layer = guac_display_default_layer(display);
        guac_display_layer_resize(layer, width, height);

        /* Prime the display with a first full frame so that the measured
         * frames are all steady state incremental updates */
        bench_paint(layer, "fullscreen", 0, width, height);
        guac_display_end_frame(display);
        bench_wait_idle(display);

        uint64_t wall_start = bench_now();
        uint64_t cpu_start = bench_cpu();

        uint64_t deadline = wall_start;

        for (int frame = 1; frame <= frames; frame++) {

            bench_paint(layer, scenario, frame, width, height);
            guac_display_end_frame(display);
            bench_wait_idle(display);

            /* Holding the frame rate steady makes the encoding decisions
             * (which depend on how often a region is updated) independent of
             * how fast the build under test happens to be, so that two builds
             * can be compared by their output rather than only by their
             * timings. Timings are meaningless in this mode. */
            if (pace) {
                deadline += (uint64_t) pace * 1000;
                uint64_t now = bench_now();
                if (now < deadline) {
                    uint64_t remaining = deadline - now;
                    struct timespec interval = {
                        .tv_sec = remaining / 1000000,
                        .tv_nsec = (remaining % 1000000) * 1000
                    };
                    nanosleep(&interval, NULL);
                }
            }

        }

        uint64_t wall = bench_now() - wall_start;
        uint64_t cpu = bench_cpu() - cpu_start;

        if (!csv)
            printf("%-12s %8i %10.1f %10.1f %10.3f %10.3f\n", scenario, frames,
                    wall / 1000.0, cpu / 1000.0,
                    wall / 1000.0 / frames, cpu / 1000.0 / frames);
        else
            printf("%s,%i,%.1f,%.1f,%.3f,%.3f\n", scenario, frames,
                    wall / 1000.0, cpu / 1000.0,
                    wall / 1000.0 / frames, cpu / 1000.0 / frames);

        fflush(stdout);

        /* Stop and free the display, which joins all worker threads. The
         * guac_client itself is intentionally left allocated: this is a
         * short-lived benchmark, and guac_client_free() expects a client that
         * went through the normal connection lifecycle. */
        guac_display_stop(display);
        guac_display_free(display);
        guac_socket_free(client->socket);
        close(fd);

    }

    free(source);
    return 0;

}
