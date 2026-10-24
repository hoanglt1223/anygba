#pragma once

#include "anygba/video_options.h"
#include "video/image.h"

// Pixel-art scalers on one layer with transparent pixels (alpha 0). The scalers only know
// opaque colours, so the colours are scaled with transparent pixels filled from an opaque
// neighbour (no dark fringes), and the coverage is scaled as a black/white mask through the same
// scaler, which rounds silhouettes the way it rounds colour edges.
namespace anygba::video::hd {

// out = in scaled by factor(s) (a copy for factor-1 scalers).
void scale_layer(Scaler s, const Image& in, Image& out);

// Resamples a scaled layer to exactly w x h: a copy when the sizes match, else nearest
// neighbour. Alpha is kept.
void fit_layer(const Image& in, int w, int h, Image& out);

}  // namespace anygba::video::hd
