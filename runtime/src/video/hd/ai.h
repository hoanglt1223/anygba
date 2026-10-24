#pragma once

#include "anygba/video_options.h"
#include "video/image.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// AI upscaling through ONNX Runtime (docs/user/HD-PACKS.md, "AI upscaling"). ONNX Runtime is
// loaded at run time, so games never depend on it: from ANYGBA_ONNXRUNTIME (a library file or a
// folder), next to the program, or the model cache, where `anygba hdpack fetch-runtime` puts the
// pinned DirectML build on Windows. Models are downloaded on first use into the cache, checked
// against pinned SHA-256 hashes, and never shipped.
namespace anygba::video::hd::ai {

struct ModelInfo {
    AiModel model;
    const char* name;      // "general-x4v3"
    const char* licence;   // of the weights
    const char* source;    // where the weights come from
    const char* url;       // archive downloaded on first use
    const char* sha256;    // of the archive
    const char* onnx;      // model file inside the archive
    int factor;            // upscaling factor
};
const ModelInfo& info(AiModel model);

// Downloads on first use can be watched and stopped from another thread. `progress` gets lines
// such as "ONNX Runtime: 3.1 MB" about once a second; once `cancel` is set the download stops,
// its partial file is removed, and the call fails with the error kCancelled.
struct Fetch {
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(const std::string&)> progress;
};
inline bool cancelled(const Fetch& f) { return f.cancel && *f.cancel; }

// Downloads `url` (curl) to `file` unless a file with that SHA-256 is already there; `what` names
// it in progress lines. ANYGBA_AI_MIRROR=<base URL> fetches <base URL>/<file name of url> instead
// (offline mirrors and tests). Nothing is left at `file` unless the hash matches. Returns an error.
std::optional<std::string> download(const std::string& url, const std::string& sha256, const std::filesystem::path& file,
                                    const std::string& what, const Fetch& fetch = {});

// The cache folder: ANYGBA_AI_CACHE, else <per-user data folder>/ai.
std::filesystem::path default_cache(const std::optional<std::filesystem::path>& data_dir);

// Downloads (curl) and unpacks (tar) the model into the cache if it is not there yet, checking the
// archive's SHA-256, and stores a copy that accepts any picture size. Returns that .onnx path, or
// nullopt with `error` set.
std::optional<std::filesystem::path> ensure_model(AiModel model, const std::filesystem::path& cache, std::string* error,
                                                  const Fetch& fetch = {});

// Windows x64: downloads the pinned ONNX Runtime (DirectML build) into the cache if missing, and
// with `directml_redistributable` also Microsoft's DirectML redistributable (for Windows versions
// whose own DirectML.dll is too old). Returns the library's path. Elsewhere: an error naming
// ANYGBA_ONNXRUNTIME.
std::optional<std::filesystem::path> fetch_runtime(const std::filesystem::path& cache, bool directml_redistributable,
                                                   std::string* error, const Fetch& fetch = {});

enum class Device : std::uint8_t { Auto, DirectML, Cuda, Cpu };
std::optional<Device> parse_device(std::string_view name);
const char* device_name(Device d);

class Upscaler {
public:
    // Loads ONNX Runtime and the model (downloading it if needed). Auto tries DirectML, then CUDA,
    // then the CPU. Returns null with `error` set when anything is missing.
    static std::unique_ptr<Upscaler> create(AiModel model, const std::filesystem::path& cache, Device device,
                                            std::string* error, const Fetch& fetch = {});
    virtual ~Upscaler() = default;

    // out = in upscaled by factor(). Pictures larger than the tile size are processed in
    // overlapping tiles. Alpha (transparency) is scaled separately, by nearest neighbour.
    virtual void upscale(const Image& in, Image& out) = 0;
    virtual int factor() const = 0;
    // The execution provider in use ("DirectML", "CUDA", "CPU").
    virtual const std::string& device() const = 0;
};

}  // namespace anygba::video::hd::ai
