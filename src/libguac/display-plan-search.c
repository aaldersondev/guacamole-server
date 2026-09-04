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

#include "display-plan.h"
#include "display-priv.h"
#include "guacamole/display.h"
#include "guacamole/rect.h"

#include <string.h>
#include <stdint.h>

/**
 * Stores the given operation within the ops_by_hash table of the given display
 * plan based on the given hash value. The hash function applied for storing
 * the operation is GUAC_DISPLAY_PLAN_OPERATION_HASH(). If another operation is
 * already stored at the same location within ops_by_hash, that operation will
 * be replaced.
 *
 * @param plan
 *     The plan to store the operation within.
 *
 * @param hash
 *     The hash value to use to calculate the storage location. This value will
 *     be further hashed with GUAC_DISPLAY_PLAN_OPERATION_HASH().
 *
 * @param op
 *     The operation to store.
 */
static void guac_display_plan_store_indexed_op(guac_display_plan* plan, uint64_t hash,
        guac_display_plan_operation* op) {

    size_t index = GUAC_DISPLAY_PLAN_OPERATION_HASH(hash);

    /* Whether a bucket is in use is tracked by the occupancy bitmap rather
     * than by the contents of the bucket itself. Only the bitmap needs to be
     * cleared for each new frame, sparing a megabyte of memset per frame. */
    uint32_t* occupied = &(plan->ops_by_hash_occupied[index / 32]);
    uint32_t bit = (uint32_t) 1 << (index % 32);

    if (!(*occupied & bit)) {
        guac_display_plan_indexed_operation* entry = &(plan->ops_by_hash[index]);
        entry->hash = hash;
        entry->op = op;
        *occupied |= bit;
    }

}

/**
 * Removes and returns a pointer to the matching operation stored within the
 * ops_by_hash table of the given display plan, if any. If no such operation is
 * stored, NULL is returned.
 *
 * @param plan
 *     The plan to retrieve the operation from.
 *
 * @param hash
 *     The hash value to use to calculate the storage location. This value will
 *     be further hashed with GUAC_DISPLAY_PLAN_OPERATION_HASH().
 *
 * @return
 *     The operation that was stored under the given hash, if any, or NULL if
 *     no such operation was found.
 */
static guac_display_plan_operation* guac_display_plan_remove_indexed_op(guac_display_plan* plan, uint64_t hash) {

    size_t index = GUAC_DISPLAY_PLAN_OPERATION_HASH(hash);

    /* Answer the common case (an empty bucket) from the small occupancy
     * bitmap, leaving the megabyte-sized index untouched. This function is
     * called once per pixel of the changed region, and all but a few hundred
     * of those calls find nothing. */
    uint32_t occupied = plan->ops_by_hash_occupied[index / 32];
    if (!(occupied & ((uint32_t) 1 << (index % 32))))
        return NULL;

    guac_display_plan_indexed_operation* entry = &(plan->ops_by_hash[index]);

    /* NOTE: We verify the hash value here because the lookup performed is
     * actually a hash of a hash. There's an additional chance of collisions
     * between hash values at this second level of hashing. */

    guac_display_plan_operation* op = entry->op;
    if (op != NULL && entry->hash == hash) {
        entry->op = NULL;
        plan->ops_by_hash_occupied[index / 32] =
            occupied & ~((uint32_t) 1 << (index % 32));
        return op;
    }

    return NULL;

}

/**
 * Calculates the hash of the 64x64 block of image data at the given location
 * within the given layer state.
 *
 * IMPORTANT: The value produced here must be identical to the value that
 * guac_display_plan_search_layer() produces for a matching 64x64 window, or
 * reusable image data will never be recognized. The two are equivalent because
 * the hash function shifts left by one bit per value consumed: after 64 values,
 * the contribution of the oldest value has been shifted out of the 64-bit
 * accumulator entirely, which is exactly what makes the sliding window in
 * guac_display_plan_search_layer() work.
 *
 * @param layer_state
 *     The layer state containing the image buffer to hash.
 *
 * @param rect
 *     The 64x64 rectangle within that image buffer to hash.
 *
 * @return
 *     The hash of the image data within the given rectangle.
 */
static uint64_t guac_display_cell_hash(
        const guac_display_layer_state* layer_state, const guac_rect* rect) {

    size_t stride = layer_state->buffer_stride;
    const unsigned char* data = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(*layer_state, *rect);

    /* Each value consumed by the hash depends on the value before it, so
     * hashing a single row at a time leaves the processor waiting on that
     * dependency for most of its cycles. Four rows are hashed at once instead:
     * the four chains are independent of each other, and their results are
     * still folded together in row order, so the value produced is unchanged.
     *
     * GUAC_DISPLAY_CELL_SIZE is a multiple of four by definition (it is a
     * power of two, and its exponent is GUAC_DISPLAY_CELL_SIZE_EXPONENT). */

    uint64_t cell_hash = 0;
    for (int y = 0; y < GUAC_DISPLAY_CELL_SIZE; y += 4) {

        /* Get next four rows */
        const uint32_t* row_a = (const uint32_t*) data;
        const uint32_t* row_b = (const uint32_t*) (data + stride);
        const uint32_t* row_c = (const uint32_t*) (data + stride * 2);
        const uint32_t* row_d = (const uint32_t*) (data + stride * 3);
        data += stride * 4;

        uint64_t hash_a = 0, hash_b = 0, hash_c = 0, hash_d = 0;
        for (int x = 0; x < GUAC_DISPLAY_CELL_SIZE; x++) {
            hash_a = ((hash_a * 31) << 1) + *(row_a++);
            hash_b = ((hash_b * 31) << 1) + *(row_b++);
            hash_c = ((hash_c * 31) << 1) + *(row_c++);
            hash_d = ((hash_d * 31) << 1) + *(row_d++);
        }

        /* Incorporate row hash values into overall cell hash, in row order */
        cell_hash = ((cell_hash * 31) << 1) + hash_a;
        cell_hash = ((cell_hash * 31) << 1) + hash_b;
        cell_hash = ((cell_hash * 31) << 1) + hash_c;
        cell_hash = ((cell_hash * 31) << 1) + hash_d;

    }

    return cell_hash;

}

/**
 * Initializes the given rectangle with the bounds of the pending frame cell
 * containing the given coordinate.
 *
 * @param rect
 *     The rectangle to initialize.
 *
 * @param x
 *     The X coordinate of the point that the rectangle must contain.
 *
 * @param y
 *     The Y coordinate of the point that the rectangle must contain.
 */
static void guac_display_cell_init_rect(guac_rect* rect, int x, int y) {
    x = (x / GUAC_DISPLAY_CELL_SIZE) * GUAC_DISPLAY_CELL_SIZE;
    y = (y / GUAC_DISPLAY_CELL_SIZE) * GUAC_DISPLAY_CELL_SIZE;
    guac_rect_init(rect, x, y, GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE);
}

void PFR_guac_display_plan_index_dirty_cells(guac_display_plan* plan) {

    memset(plan->ops_by_hash_occupied, 0, sizeof(plan->ops_by_hash_occupied));

    guac_display_plan_operation* op = plan->ops;
    for (int i = 0; i < plan->length; i++) {

        if (op->type == GUAC_DISPLAY_PLAN_OPERATION_IMG) {

            guac_display_layer* layer = op->layer;

            guac_rect layer_bounds;
            guac_display_layer_get_bounds(layer, &layer_bounds);

            guac_rect cell;
            guac_display_cell_init_rect(&cell, op->dest.left, op->dest.top);

            guac_rect_constrain(&cell, &layer_bounds);
            if (guac_rect_width(&cell) == GUAC_DISPLAY_CELL_SIZE
                    && guac_rect_height(&cell) == GUAC_DISPLAY_CELL_SIZE) {
                guac_display_plan_store_indexed_op(plan,
                        guac_display_cell_hash(&layer->pending_frame, &cell), op);
            }

        }

        op++;

    }

}

/**
 * Compares two rectangular regions of two arbitrary buffers, returning whether
 * those regions contain identical data.
 *
 * @param data_a
 *     A pointer to the first byte of image data within the first region being
 *     compared.
 *
 * @param width_a
 *     The width of the first region, in pixels.
 *
 * @param height_a
 *     The height of the first region, in pixels.
 *
 * @param stride_a
 *     The number of bytes in each row of image data in the first region.
 *
 * @param data_b
 *     A pointer to the first byte of image data within the second region being
 *     compared.
 *
 * @param width_b
 *     The width of the second region, in pixels.
 *
 * @param height_b
 *     The height of the second region, in pixels.
 *
 * @param stride_b
 *     The number of bytes in each row of image data in the first region.
 *
 * @return
 *     Non-zero if the regions contain at least one differing pixel, zero
 *     otherwise.
 */
static int guac_image_cmp(const unsigned char* restrict data_a, int width_a, int height_a,
        int stride_a, const unsigned char* restrict data_b, int width_b, int height_b,
        int stride_b) {

    int y;

    /* If core dimensions differ, just compare those. Done. */
    if (width_a != width_b) return width_a - width_b;
    if (height_a != height_b) return height_a - height_b;

    size_t length = guac_mem_ckd_mul_or_die(width_a, GUAC_DISPLAY_LAYER_RAW_BPP);

    for (y = 0; y < height_a; y++) {

        /* Compare row. If different, use that result. */
        int cmp_result = memcmp(data_a, data_b, length);
        if (cmp_result != 0)
            return cmp_result;

        /* Next row */
        data_a += stride_a;
        data_b += stride_b;

    }

    /* Otherwise, same. */
    return 0;

}

/**
 * Searches the ops_by_hash table of the given display plan for occurrences of
 * the given hash, replacing the matching operation with a copy operation if a
 * match is found.
 *
 * NOTE: While this function will search for and optimize operations that copy
 * existing data, it can only do so for distinct image data. Multiple
 * operations that copy the same exact data (like a region tiled with multiple
 * copies of some pattern) can only be stored in the table once, and therefore
 * will only match once.
 *
 * @param plan
 *     The display plan to update with any copies found.
 *
 * @param copy_from_layer
 *     The layer whose previous frame is being searched.
 *
 * @param x
 *     The X coordinate of the upper-left corner of the 64x64 region currently
 *     being checked.
 *
 * @param y
 *     The Y coordinate of the upper-left corner of the 64x64 region currently
 *     being checked.
 *
 * @param hash
 *     The hash value that applies to the 64x64 rectangle at the given
 *     coordinates.
 */
static inline void PFR_LFR_guac_display_plan_find_copies(guac_display_plan* plan,
        guac_display_layer* copy_from_layer, int x, int y, uint64_t hash) {

    /* Transform the matching operation into a copy of the current region if
     * any operations match, banning the underlying hash from further checks if
     * a collision occurs */
    guac_display_plan_operation* op = guac_display_plan_remove_indexed_op(plan, hash);
    if (op != NULL) {

        guac_display_layer* copy_to_layer = op->layer;

        guac_rect src_rect;
        guac_rect_init(&src_rect, x, y, GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE);

        guac_rect dst_rect;
        guac_display_cell_init_rect(&dst_rect, op->dest.left, op->dest.top);

        const unsigned char* copy_from = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(copy_from_layer->last_frame, src_rect);
        const unsigned char* copy_to = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(copy_to_layer->pending_frame, dst_rect);

        /* Only transform into a copy if the image data is truly identical (not a collision) */
        if (!guac_image_cmp(copy_from, GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE, copy_from_layer->last_frame.buffer_stride,
                copy_to, GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE, copy_to_layer->pending_frame.buffer_stride)) {
            op->type = GUAC_DISPLAY_PLAN_OPERATION_COPY;
            op->src.layer_rect.layer = copy_from_layer->last_frame_buffer;
            op->src.layer_rect.rect = src_rect;
            op->dest = dst_rect;
        }

    }

}

/**
 * Searches the previous frame of the given layer for image data that can be
 * reused by operations in the given plan, rewriting any such operation as a
 * copy. A 64x64 window is slid over every pixel position of the given region,
 * and the hash of the image data within that window is looked up in the plan's
 * index of pending operations.
 *
 * The window slides for free: because the hash function shifts its accumulator
 * left by one bit per value consumed, the contribution of any value falls out
 * of the 64-bit accumulator after 64 further values. Running hashes are
 * therefore maintained by simply continuing to accumulate, with one running
 * hash per column of the region.
 *
 * @param plan
 *     The display plan to update with any copies found.
 *
 * @param layer
 *     The layer whose previous frame should be searched.
 *
 * @param rect
 *     The region of that previous frame to search.
 */
static void PFR_LFR_guac_display_plan_search_layer(guac_display_plan* plan,
        guac_display_layer* layer, const guac_rect* rect) {

    const guac_display_layer_state* layer_state = &(layer->last_frame);

    size_t stride = layer_state->buffer_stride;
    const unsigned char* data = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(*layer_state, *rect);

    /* NOTE: Because the hash value of the sliding 64x64 window is available
     * only upon reaching the bottom-right corner of that window, we offset the
     * coordinates here by the relative location of the bottom-right corner
     * (GUAC_DISPLAY_CELL_SIZE - 1) so that we have easy access to the
     * coordinates of the upper-left corner of the sliding window.
     *
     * This also allows us to easily determine when the hash is valid and it's
     * safe to check for a match. Once the coordinates are within the given
     * rect, we have evaluated a full 64x64 rectangle and have a valid hash. */

    int start_x = rect->left   - GUAC_DISPLAY_CELL_SIZE + 1;
    int end_x   = rect->right  - GUAC_DISPLAY_CELL_SIZE + 1;
    int start_y = rect->top    - GUAC_DISPLAY_CELL_SIZE + 1;
    int end_y   = rect->bottom - GUAC_DISPLAY_CELL_SIZE + 1;

    if (end_x <= start_x || end_y <= start_y)
        return;

    /* Only the columns actually covered by this search need to be cleared */
    size_t columns = end_x - start_x;
    uint64_t cell_hash[GUAC_DISPLAY_MAX_WIDTH];
    memset(cell_hash, 0, columns * sizeof(uint64_t));

    for (int y = start_y; y < end_y; y++) {

        /* Get current row */
        const uint32_t* row = (const uint32_t*) data;
        data += stride;

        uint64_t row_hash = 0;
        uint64_t* current_cell_hash = cell_hash;

        /* Windows are only complete once 64 rows and 64 columns have been
         * consumed. Rather than testing for that per pixel, split each row
         * into the part that merely fills the window and the part that can
         * actually be checked for matches. */
        int checked_from = end_x;
        if (y >= rect->top && rect->left < end_x)
            checked_from = rect->left;

        int x = start_x;

        for (; x < checked_from; x++) {
            row_hash = ((row_hash * 31) << 1) + *(row++);
            *current_cell_hash = ((*current_cell_hash * 31) << 1) + row_hash;
            current_cell_hash++;
        }

        for (; x < end_x; x++) {
            row_hash = ((row_hash * 31) << 1) + *(row++);
            uint64_t hash = ((*current_cell_hash * 31) << 1) + row_hash;
            *(current_cell_hash++) = hash;
            PFR_LFR_guac_display_plan_find_copies(plan, layer, x, y, hash);
        }

    }

}

void PFR_LFR_guac_display_plan_rewrite_as_copies(guac_display_plan* plan) {

    guac_display* display = plan->display;
    guac_display_layer* current = display->last_frame.layers;
    while (current != NULL) {

        /* Search only the layers that are specifically noted as possible
         * sources for copies */
        if (current->pending_frame.search_for_copies) {

            guac_rect search_region;
            guac_rect_init(&search_region, 0, 0, current->last_frame.width, current->last_frame.height);

            /* Avoid excessive computation by restricting the search region to only
             * the area that was changed in the upcoming frame (in the case of
             * scrolling, absolutely all data relevant to the scroll will have been
             * modified) */
            guac_rect_constrain(&search_region, &current->pending_frame.dirty);

            PFR_LFR_guac_display_plan_search_layer(plan, current, &search_region);
        }

        current = current->last_frame.next;

    }

}
