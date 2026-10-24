#pragma once

#include "anygba/system.hpp"
#include "anygba/video_options.h"
#include "video/hd/ai.h"
#include "video/hd/dump.h"
#include "video/hd/frame.h"
#include "video/hd/renderer.h"
#include "video/image.h"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Everything HD for one running game: captures each frame as it is drawn and, when any HD
// setting is on, renders it through the HD renderer with the game's pack, dumper, widescreen
// rules and AI upscaler. Settings change at once (set_options), without a restart.
namespace anygba::video::hd {

class Pack;
class Rules;

struct SessionSetup {
    std::string game_code;  // ROM header code, e.g. "BPEE"
    std::string title;
    // Folders searched for hdpacks/<code>/ and widescreen/<code>.toml, in order.
    std::vector<std::filesystem::path> search_dirs;
    std::optional<std::filesystem::path> pack_dir;  // --hd-pack: this folder instead
    std::optional<std::filesystem::path> dump_dir;  // --hd-dump: dump here
    std::optional<std::filesystem::path> data_dir;  // default dump folder and AI cache
    DumpFilter dump_filter;
    ai::Device ai_device = ai::Device::Auto;
};

class Session {
public:
    explicit Session(SessionSetup setup);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void set_options(const Options& options);
    // Whether the HD path is in use with the current settings.
    bool active() const;

    // System::on_line_rendered. Renders the HD picture after the last line.
    void on_line(System& sys, unsigned line);
    // The latest HD picture (RGBA), or null when inactive or not drawn yet.
    const Image* picture();
    // GBA pixels across the picture: 240, or wider in widescreen.
    int logical_width() const;
    static constexpr int logical_height() { return kScreenHeight; }

    // Once per frame on the game thread: applies pack hot reloads and writes the dump index now and then.
    void between_frames();
    // Status and warnings for the on-screen display. Download progress lines start with
    // kAiDownloadPrefix; each replaces the previous one.
    std::vector<std::string> take_messages();
    static constexpr const char* kAiDownloadPrefix = "AI download: ";
    // ROM patches of the widescreen rules (empty unless widescreen is on).
    std::map<std::uint32_t, std::uint16_t> rom_patches() const;
    // Name of the widescreen scene of the last frame ("" when widescreen is off).
    const std::string& scene() const { return scene_; }
    // Average milliseconds per HD render and per AI upscale (0 when off).
    double render_ms() const { return frames_ ? render_ms_ / static_cast<double>(frames_) : 0; }
    double ai_ms() const;

private:
    void message(std::string text);
    void open_pack();
    void ai_worker();
    void stop_ai();

    SessionSetup setup_;
    Options options_;
    std::unique_ptr<Recorder> recorder_;
    Renderer renderer_;
    std::unique_ptr<Pack> pack_;
    std::unique_ptr<Dumper> dumper_;
    std::unique_ptr<Rules> rules_;
    Image picture_;
    bool have_picture_ = false;
    std::string scene_;
    bool trace_scenes_ = false;  // ANYGBA_WIDESCREEN_TRACE: print scene changes (for rule authors)
    double render_ms_ = 0;
    std::uint64_t frames_ = 0;

    std::mutex messages_mutex_;
    std::vector<std::string> messages_;

    // AI: a worker thread loads the model (downloading ONNX Runtime and the model on first use;
    // turning AI off cancels), then upscales the newest frame it is given.
    struct AiState;
    std::unique_ptr<AiState> ai_;
    Image ai_picture_;
    bool have_ai_picture_ = false;
};

}  // namespace anygba::video::hd
