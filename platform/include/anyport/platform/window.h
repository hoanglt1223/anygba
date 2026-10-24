#pragma once

#include <cstdint>
#include <span>
#include <string>
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
};

// Window output read back as tightly packed 8-bit R, G, B, A bytes.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

// Draws on top of the picture and decides where the picture goes (touch controls). All
// coordinates are output pixels; `safe` is the part of the output not covered by notches,
// rounded corners or system bars.
class Overlay {
public:
    virtual ~Overlay() = default;
    virtual Rect picture_rect(float width, float height, Rect safe) = 0;
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

    // While an overlay is set it places the picture instead of the integer-scale letterbox.
    // The overlay must outlive the window or be reset with nullptr first.
    void set_overlay(Overlay* overlay);

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    Overlay* overlay_ = nullptr;
    int width_;
    int height_;
};

}  // namespace anyport::platform
