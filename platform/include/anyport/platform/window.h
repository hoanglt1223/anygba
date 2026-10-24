#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace anyport::platform {

enum class PixelFormat {
    // 16-bit, red in bits 0-4, green in 5-9, blue in 10-14, bit 15 ignored.
    Rgb555,
};

struct WindowConfig {
    std::string title;
    int width = 0;  // framebuffer size in pixels
    int height = 0;
    PixelFormat format = PixelFormat::Rgb555;
    int scale = 3;  // initial window size as a multiple of the framebuffer
    bool vsync = true;
    bool hidden = false;  // never shown (GPU tests)
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

enum class Scaling { Nearest, Linear };

// A picture the size of the destination rectangle that the shown picture is multiplied by
// (R, G, B, A bytes; white leaves it alone). It is uploaded again only when `version` or the
// size changes.
struct Overlay {
    std::span<const std::uint32_t> pixels;
    int width = 0;
    int height = 0;
    std::uint64_t version = 0;
};

// Window output read back as tightly packed 8-bit R, G, B, A bytes.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct FRect {
    float x = 0, y = 0, w = 0, h = 0;
};

// Draws on top of the picture and decides where the picture goes (touch controls). All
// coordinates are output pixels; `safe` is the part of the output not covered by notches,
// rounded corners or system bars.
class ScreenOverlay {
public:
    virtual ~ScreenOverlay() = default;
    virtual FRect picture_rect(float width, float height, FRect safe) = 0;
    virtual void draw(SDL_Renderer* renderer, float width, float height) = 0;
};

// A resizable window showing one framebuffer, scaled by the largest integer factor that
// fits and letterboxed with black.
class Window {
public:
    explicit Window(const WindowConfig& config);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // `pixels` holds width * height tightly packed pixels. When `capture` is set, the
    // composed output is read back before it is shown.
    void present(std::span<const std::uint16_t> pixels, Image* capture = nullptr);

    // Draws a filtered picture of any size (R, G, B, A bytes per pixel) into `dst` of the
    // render target, black around it. The renderer (the GPU unless it is SDL's software
    // renderer) does the scaling.
    void present_rgba(std::span<const std::uint32_t> pixels, int width, int height, Rect dst, Scaling scaling,
                      const Overlay* overlay = nullptr, Image* capture = nullptr);
    // Pixels present_rgba draws into: the fixed render size if set, else the window's.
    std::pair<int, int> render_size() const;
    // The window's size in pixels, whatever the render size.
    std::pair<int, int> output_pixels() const;
    // Shows a picture of output_pixels() size over the whole window, pixel for pixel (the
    // in-game menu): no render size, letterbox or screen overlay.
    void present_ui(std::span<const std::uint32_t> pixels, int width, int height);
    // Render into an offscreen target of this size, shown scaled to the window with the aspect
    // ratio kept; (0, 0) renders straight to the window.
    void set_render_size(int width, int height);
    void set_fullscreen(bool on);
    void set_window_size(int width, int height);
    // False for SDL's software renderer (no GPU).
    bool accelerated() const;
    // Refresh rate of the display the window is on (it changes when the window moves to
    // another monitor), or 0 when unknown.
    double refresh_rate() const;
    // The primary display's desktop mode, or (0, 0) when unknown.
    static std::pair<int, int> desktop_size();
    // While an overlay is set it places the picture instead of the integer-scale letterbox.
    // The overlay must outlive the window or be reset with nullptr first.
    void set_overlay(ScreenOverlay* overlay);

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

private:
    std::pair<float, float> output_size() const;
    // Where the screen overlay puts the picture in an output of w x h pixels.
    FRect screen_picture_rect(float w, float h) const;

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_Texture* rgba_ = nullptr;    // present_rgba upload
    SDL_Texture* target_ = nullptr;  // fixed render size
    SDL_Texture* ui_ = nullptr;      // present_ui upload
    int ui_width_ = 0, ui_height_ = 0;
    int rgba_width_ = 0, rgba_height_ = 0;
    SDL_Texture* overlay_ = nullptr;
    int overlay_width_ = 0, overlay_height_ = 0;
    std::uint64_t overlay_version_ = 0;
    bool logical_ = true;            // logical presentation is on (present())
    ScreenOverlay* screen_overlay_ = nullptr;
    int width_;
    int height_;
};

}  // namespace anyport::platform
