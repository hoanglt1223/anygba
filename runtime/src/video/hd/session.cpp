#include "video/hd/session.h"

#include "video/hd/pack.h"
#include "video/hd/widescreen.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <system_error>
#include <thread>

namespace anygba::video::hd {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

struct Session::AiState {
    AiModel model = AiModel::GeneralX4v3;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    std::atomic<bool> cancel{false};  // stops a download of ONNX Runtime or the model
    std::atomic<bool> loaded{false};
    std::unique_ptr<ai::Upscaler> upscaler;
    Image job;
    bool has_job = false;
    Image ready;
    bool fresh = false;
    double total_ms = 0;
    std::uint64_t count = 0;
};

Session::Session(SessionSetup setup) : setup_(std::move(setup)), recorder_(std::make_unique<Recorder>()) {
    const char* trace = std::getenv("ANYGBA_WIDESCREEN_TRACE");
    trace_scenes_ = trace && *trace && std::string(trace) != "0";
    std::vector<fs::path> dirs;
    for (const fs::path& d : setup_.search_dirs) dirs.push_back(d / "widescreen");
    try {
        fs::path found;
        if (auto rules = Rules::find(setup_.game_code, dirs, &found)) {
            rules_ = std::make_unique<Rules>(std::move(*rules));
        }
    } catch (const std::exception& e) {
        message(std::string("widescreen rules ignored: ") + e.what());
    }
}

Session::~Session() { stop_ai(); }

void Session::message(std::string text) {
    std::lock_guard lock(messages_mutex_);
    messages_.push_back(std::move(text));
}

std::vector<std::string> Session::take_messages() {
    if (pack_) {
        for (auto& w : pack_->take_warnings()) message("HD pack: " + w);
    }
    std::lock_guard lock(messages_mutex_);
    return std::exchange(messages_, {});
}

void Session::open_pack() {
    std::vector<fs::path> candidates;
    if (setup_.pack_dir) {
        candidates.push_back(*setup_.pack_dir);
    } else {
        for (const fs::path& d : setup_.search_dirs) candidates.push_back(d / "hdpacks" / setup_.game_code);
    }
    for (const fs::path& dir : candidates) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        try {
            pack_ = std::make_unique<Pack>(dir);
            pack_->watch();
            message("HD pack " + (pack_->title().empty() ? utf8(dir) : pack_->title()) + ": " +
                    std::to_string(pack_->size()) + " images");
        } catch (const std::exception& e) {
            message(e.what());
        }
        return;
    }
    if (setup_.pack_dir) message("HD pack folder not found: " + utf8(*setup_.pack_dir));
}

void Session::set_options(const Options& o) {
    const Options before = options_;
    options_ = o;
    if (o.hd_pack && !pack_) {
        open_pack();
    } else if (!o.hd_pack) {
        pack_.reset();
    }
    if (o.hd_dump && !dumper_) {
        fs::path dir;
        if (setup_.dump_dir) {
            dir = *setup_.dump_dir;
        } else if (setup_.data_dir) {
            dir = *setup_.data_dir / "hdpacks-dump" / setup_.game_code;
        }
        if (dir.empty()) {
            message("HD dump: no folder; pass --hd-dump DIR");
        } else {
            try {
                dumper_ = std::make_unique<Dumper>(dir, setup_.game_code, setup_.title, setup_.dump_filter);
                message("HD dump to " + utf8(dir) + " (" + std::to_string(dumper_->entries()) + " so far)");
            } catch (const std::exception& e) {
                message(e.what());
            }
        }
    } else if (!o.hd_dump && dumper_) {
        dumper_.reset();  // writes the index
    }
    if (o.widescreen != Options{}.widescreen && !rules_ && before.widescreen != o.widescreen) {
        message("widescreen: no rules for " + setup_.game_code + "; every layer is widened from VRAM");
    }
    if (o.ai == Ai::Off || (ai_ && ai_->model != o.ai_model)) stop_ai();
    if (o.ai != Ai::Off && !ai_) {
        ai_ = std::make_unique<AiState>();
        ai_->model = o.ai_model;
        ai_->thread = std::thread([this] { ai_worker(); });
    }
    if (!active()) have_picture_ = false;
}

bool Session::active() const {
    return options_.internal_scale > 1 || options_.widescreen != Widescreen::Off || (options_.hd_pack && pack_) ||
           dumper_ || options_.ai != Ai::Off;
}

int Session::logical_width() const { return kScreenWidth + 2 * widescreen_extra(options_.widescreen); }

std::map<std::uint32_t, std::uint16_t> Session::rom_patches() const {
    if (options_.widescreen == Widescreen::Off || !rules_) return {};
    return rules_->rom_patches();
}

void Session::on_line(System& sys, unsigned line) {
    if (line == 0) recorded_ = false;
    if (!active()) return;
    if (!recorder_->on_line(sys, line)) return;
    recorded_ = true;
    render_recorded(sys);
}

bool Session::render_again(System& sys) {
    if (!active() || !recorded_) return false;
    render_recorded(sys);
    return true;
}

void Session::render_recorded(System& sys) {
    Frame& frame = recorder_->frame();
    const int extra = widescreen_extra(options_.widescreen);
    if (extra > 0 && rules_) {
        const std::string& scene = rules_->apply(sys.memory(), frame, extra);
        if (scene != scene_ && trace_scenes_) {
            std::printf("widescreen: frame %llu scene %s\n", static_cast<unsigned long long>(frame.number), scene.c_str());
        }
        scene_ = scene;
    } else {
        frame.scene = {};
        for (auto& e : frame.extension) e = {};
        scene_ = extra > 0 ? "default" : "";
    }

    RenderConfig cfg;
    cfg.scale = options_.internal_scale;
    cfg.extra = extra;
    cfg.smooth_affine = options_.smooth_affine;
    cfg.layer_filter = options_.layer_filter;
    cfg.textures = options_.hd_pack ? pack_.get() : nullptr;
    cfg.sink = dumper_.get();
    if (options_.ai == Ai::Frame && ai_ && ai_->loaded) {
        cfg.scale = 1;  // the model upscales the finished 1x picture
    } else if (options_.ai == Ai::Layers && ai_ && ai_->loaded) {
        cfg.scale = ai_->upscaler->factor();
        cfg.layer_upscaler = [this](const Image& in, Image& out) { ai_->upscaler->upscale(in, out); };
    }
    const auto start = std::chrono::steady_clock::now();
    renderer_.render(frame, cfg, picture_);
    render_ms_ += ms_since(start);
    have_picture_ = true;
    ++frames_;

    if (options_.ai == Ai::Frame && ai_ && ai_->loaded) {
        std::lock_guard lock(ai_->mutex);
        ai_->job = picture_;  // the newest frame replaces one still waiting
        ai_->has_job = true;
        ai_->cv.notify_one();
    }
}

const Image* Session::picture() {
    if (!active() || !have_picture_) return nullptr;
    if (options_.ai == Ai::Frame && ai_) {
        {
            std::lock_guard lock(ai_->mutex);
            if (ai_->fresh) {
                std::swap(ai_picture_, ai_->ready);
                ai_->fresh = false;
                have_ai_picture_ = true;
            }
        }
        if (have_ai_picture_ && ai_picture_.height == picture_.height * ai_->upscaler->factor()) return &ai_picture_;
    }
    return &picture_;
}

void Session::between_frames() {
    if (pack_ && pack_->apply_changes()) message("HD pack reloaded: " + std::to_string(pack_->size()) + " images");
    if (dumper_ && frames_ % 600 == 599) {
        if (const auto error = dumper_->write_index()) message(*error);
    }
}

double Session::ai_ms() const {
    if (!ai_) return 0;
    std::lock_guard lock(ai_->mutex);
    return ai_->count ? ai_->total_ms / static_cast<double>(ai_->count) : 0;
}

void Session::ai_worker() {
    AiState& s = *ai_;
    std::string error;
    const fs::path cache = ai::default_cache(setup_.data_dir);
    message(std::string("AI upscaling: loading ") + ai::info(s.model).name + "...");
    ai::Fetch fetch;
    fetch.cancel = &s.cancel;
    fetch.progress = [this](const std::string& line) { message(kAiDownloadPrefix + line + " (turn AI off to cancel)"); };
    auto upscaler = ai::Upscaler::create(s.model, cache, setup_.ai_device, &error, fetch);
    if (!upscaler) {
        // The renderer keeps drawing without AI (ai_->loaded stays false).
        message(s.cancel ? std::string("AI upscaling cancelled; picture without AI")
                         : "AI upscaling unavailable: " + error + "; picture without AI");
        return;
    }
    message(std::string("AI upscaling: ") + ai::info(s.model).name + " on " + upscaler->device());
    {
        std::lock_guard lock(s.mutex);
        s.upscaler = std::move(upscaler);
    }
    s.loaded = true;
    Image job, result;
    std::unique_lock lock(s.mutex);
    while (true) {
        s.cv.wait(lock, [&] { return s.stop || s.has_job; });
        if (s.stop) break;
        std::swap(job, s.job);
        s.has_job = false;
        lock.unlock();
        const auto start = std::chrono::steady_clock::now();
        s.upscaler->upscale(job, result);
        const double ms = ms_since(start);
        lock.lock();
        std::swap(s.ready, result);
        s.fresh = true;
        s.total_ms += ms;
        ++s.count;
    }
}

void Session::stop_ai() {
    if (!ai_) return;
    ai_->cancel = true;
    {
        std::lock_guard lock(ai_->mutex);
        ai_->stop = true;
    }
    ai_->cv.notify_all();
    if (ai_->thread.joinable()) ai_->thread.join();
    ai_.reset();
    have_ai_picture_ = false;
}

}  // namespace anygba::video::hd
