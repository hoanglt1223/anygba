#pragma once

#include "anygba/video_options.h"
#include "video/hd/frame.h"
#include "video/hd/keys.h"
#include "video/image.h"

#include <array>
#include <functional>
#include <memory>

// The HD PPU: renders a captured frame at an internal scale N and an optional widescreen width,
// with replacement art from an HD pack, per-layer scalers and sub-pixel rotation/scaling.
//
// Pass 1 draws every layer at 1x exactly as the scanline PPU does (and finds the replacement art
// of each tile and sprite); pass 2 rebuilds each line at N x N sub-pixels, sampling replacement art
// and affine layers per sub-pixel; then the layers are composited per sub-pixel with the line's
// windows and colour effects. Sub-pixel (0, 0) of every pixel reproduces the 1x PPU exactly.
namespace anygba::video::hd {

// Replacement art by key, resampled to the internal scale (an HD pack). Thread-safe.
class TextureSource {
public:
    virtual ~TextureSource() = default;
    // Whether any art of this type exists (lets the renderer skip hashing).
    virtual bool has(KeyType type) const = 0;
    // Art for `hash` (original size width x height) at `scale` x that size, or null. The pointer
    // stays valid until the source is reloaded, which never happens during a render.
    virtual const Texture* find(KeyType type, std::uint64_t hash, int width, int height, int scale) = 0;
};

// Receives every piece of art the renderer meets (the dumper). Thread-safe.
class ArtSink {
public:
    virtual ~ArtSink() = default;
    // `layer`: "bg0".."bg3" or "obj". Returns true the first time a key is added.
    virtual bool add(KeyType type, std::uint64_t key, std::uint64_t index_key, const Art& art, const char* layer,
                     std::uint64_t frame) = 0;
    // For a tile added for the first time: the layer around it where it was first seen
    // (`art` starts at x, y inside `context`; transparent outside the screen). Optional.
    virtual void context(KeyType type, std::uint64_t key, const Art& context, int x, int y) = 0;
};

// Upscales one layer picture (RGBA, alpha 0 = transparent) by an integer factor it chooses.
using LayerUpscaler = std::function<void(const Image& in, Image& out)>;

struct RenderConfig {
    int scale = 1;  // internal scale N
    int extra = 0;  // widescreen columns added on each side, at 1x
    bool smooth_affine = true;
    Scaler layer_filter = Scaler::Nearest;
    LayerUpscaler layer_upscaler;  // replaces layer_filter when set (AI per layer)
    TextureSource* textures = nullptr;
    ArtSink* sink = nullptr;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // out becomes (240 + 2 * extra) * scale x 160 * scale, opaque RGBA.
    void render(const Frame& frame, const RenderConfig& config, Image& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anygba::video::hd
