#pragma once

// Word indices in the shared .capture_config payload.
#define HG_CAPTURE_LAYER_SLOT 31
#define HG_CAPTURE_BOUNDS_SLOT 32
#define HG_CAPTURE_ROUND_SLOT 33
#define HG_CAPTURE_VIEW_SLOT 34
#define HG_CAPTURE_CLIP_SUPERELLIPSE 35
#define HG_CAPTURE_OPACITY 36
#define HG_CAPTURE_CLIP_RRECT 37
#define HG_CAPTURE_COLOR_FILTER 38
#define HG_CAPTURE_CONFIG_WORDS 39

// Metadata carried between the synchronous Dart bridge and the raster task.
#define HG_CAPTURE_VIEW_BITS 12
#define HG_CAPTURE_TICKET_BITS 16
#define HG_CAPTURE_RESULT_OFFSET 768
#define HG_CAPTURE_RESULT_SIZE 112
#ifndef __ASSEMBLER__
#include <stdint.h>
typedef struct { float scale, x, y, w, density; } HgCaptureTransform;
typedef struct {
 double x, y, scale_x, scale_y, width, height;
 uintptr_t view;
 unsigned viewport_width, viewport_height;
} HgCapturePlacement;
typedef struct {
 uintptr_t *view_slot;
 HgCaptureTransform transform;
 HgCapturePlacement placement;
 uintptr_t layer;
 unsigned identity;
} HgCaptureResult;
#endif
