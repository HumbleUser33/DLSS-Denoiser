/*
 * This file is part of DLSS Denoiser.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

#pragma once

// The compiled shaders, embedded by shaders.rc. Shared by the engine and the
// resource script, so nothing else may live here.

#define IDS_CS_MOTION_FRAME   101
#define IDS_CS_MOTION_BLOCK   102
#define IDS_CS_MOTION_EXPAND  103
#define IDS_CS_GLOBAL_MOTION  104
#define IDS_CS_GLOBAL_STEP    105
#define IDS_CS_GLOBAL_SOLVE   106
#define IDS_CS_SCENE_SUMS     107
#define IDS_CS_SCENE_CUT      108
#define IDS_CS_CONVERT_IN     109
#define IDS_CS_CONVERT_OUT    110
#define IDS_CS_TILES          111
#define IDS_CS_BLUR_H         112
#define IDS_CS_BLUR_V         113
#define IDS_CS_TILE_MEDIAN    114
#define IDS_CS_BLUR_GRAD      115
