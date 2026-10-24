#include "video/hd/ai.h"

#include "anygba/sha256.h"
#include "video/hd/onnx_patch.h"
#include "video/hd/process.h"

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <SDL3/SDL_filesystem.h>

namespace anygba::video::hd::ai {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// Weights are Real-ESRGAN's (BSD-3-Clause); the ONNX exports with fixed 128x128 tiles are
// Qualcomm AI Hub's builds of the same checkpoints (qai-hub-models v0.65.0).
constexpr std::array kModels = {
    ModelInfo{AiModel::GeneralX4v3, "general-x4v3", "BSD-3-Clause (Real-ESRGAN, Xintao Wang et al.)",
              "realesr-general-x4v3 (xinntao/Real-ESRGAN), ONNX by Qualcomm AI Hub",
              "https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/real_esrgan_general_x4v3/"
              "releases/v0.65.0/real_esrgan_general_x4v3-onnx-float.zip",
              "fc094329b24a00c936ebc1cc4c51bc59633e7dc36f4a553344342171cbd3c7cf",
              "real_esrgan_general_x4v3-onnx-float/real_esrgan_general_x4v3.onnx", 4},
    ModelInfo{AiModel::X4Plus, "x4plus", "BSD-3-Clause (Real-ESRGAN, Xintao Wang et al.)",
              "RealESRGAN_x4plus (xinntao/Real-ESRGAN), ONNX by Qualcomm AI Hub",
              "https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/real_esrgan_x4plus/"
              "releases/v0.65.0/real_esrgan_x4plus-onnx-float.zip",
              "05aa31323c6a7e831493171cac970e398697709cae7e7ae8e0760e6da4d64ca4",
              "real_esrgan_x4plus-onnx-float/real_esrgan_x4plus.onnx", 4},
};

// ONNX Runtime 1.24.4 with DirectML for Windows x64 (MIT), and the DirectML redistributable it
// needs (Microsoft's DirectML licence), from nuget.org.
struct Package {
    const char* url;
    const char* sha256;
    std::vector<const char*> members;  // files taken from the archive into the runtime folder
};
const std::array<Package, 2>& windows_runtime() {
    static const std::array<Package, 2> kPackages = {
        Package{"https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/"
                "microsoft.ml.onnxruntime.directml.1.24.4.nupkg",
                "57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681",
                {"runtimes/win-x64/native/onnxruntime.dll", "runtimes/win-x64/native/onnxruntime_providers_shared.dll",
                 "LICENSE", "ThirdPartyNotices.txt"}},
        Package{"https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg",
                "4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9",
                {"bin/x64-win/DirectML.dll", "LICENSE.txt"}},
    };
    return kPackages;
}
constexpr const char* kRuntimeFolder = "onnxruntime-1.24.4-directml";

#ifdef _WIN32
constexpr const char* kLibrary = "onnxruntime.dll";
#elif defined(__APPLE__)
constexpr const char* kLibrary = "libonnxruntime.dylib";
#else
constexpr const char* kLibrary = "libonnxruntime.so";
#endif

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool write_file(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}

// Downloads `url` to `file` unless a file with the right hash is already there.
std::optional<std::string> download(const std::string& url, const std::string& sha256, const fs::path& file) {
    if (const auto have = read_file(file); have && sha256_hex(*have) == sha256) return std::nullopt;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path part = file;
    part += ".part";
    if (const auto error = run_process({"curl", "--location", "--fail", "--silent", "--show-error", "--output", utf8(part), url})) {
        fs::remove(part, ec);
        return "download of " + url + " failed: " + *error + " (is curl installed?)";
    }
    const auto got = read_file(part);
    if (!got || sha256_hex(*got) != sha256) {
        fs::remove(part, ec);
        return "download of " + url + " does not have the pinned SHA-256 " + sha256;
    }
    fs::rename(part, file, ec);
    if (ec) return "cannot store " + utf8(file) + ": " + ec.message();
    return std::nullopt;
}

// Unpacks a zip archive (also .nupkg) into `dir`, all of it or only `members`.
std::optional<std::string> extract(const fs::path& archive, const fs::path& dir, const std::vector<const char*>& members) {
    std::error_code ec;
    fs::create_directories(dir, ec);
#ifdef _WIN32
    // The tar of Windows 10+ (bsdtar) reads zip files; Git's GNU tar on the PATH does not.
    char system_dir[MAX_PATH];
    const UINT n = GetSystemDirectoryA(system_dir, MAX_PATH);
    std::vector<std::string> args{(n ? std::string(system_dir, n) + "\\" : std::string()) + "tar.exe", "-xf", utf8(archive),
                                  "-C", utf8(dir)};
    for (const char* m : members) args.emplace_back(m);
    return run_process(args);
#else
    std::vector<std::string> args{"unzip", "-o", "-q", utf8(archive)};
    for (const char* m : members) args.emplace_back(m);
    args.insert(args.end(), {"-d", utf8(dir)});
    if (!run_process(args)) return std::nullopt;
    std::vector<std::string> tar{"tar", "-xf", utf8(archive), "-C", utf8(dir)};
    for (const char* m : members) tar.emplace_back(m);
    return run_process(tar);
#endif
}

// ---- ONNX Runtime, loaded once per process and kept ----

struct Runtime {
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    std::string path;
};

std::mutex g_runtime_mutex;
Runtime g_runtime;

const OrtApi* open_library(const fs::path& file, std::string* error) {
#ifdef _WIN32
    // Its DirectML.dll and provider DLLs are looked up next to it first.
    HMODULE lib = LoadLibraryExW(file.wstring().c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!lib) {
        *error = "cannot load " + utf8(file) + " (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    using GetBase = const OrtApiBase*(ORT_API_CALL*)();
    const auto get = reinterpret_cast<GetBase>(reinterpret_cast<void*>(GetProcAddress(lib, "OrtGetApiBase")));
#else
    void* lib = dlopen(utf8(file).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        *error = std::string("cannot load ") + utf8(file) + ": " + dlerror();
        return nullptr;
    }
    using GetBase = const OrtApiBase* (*)();
    const auto get = reinterpret_cast<GetBase>(dlsym(lib, "OrtGetApiBase"));
#endif
    if (!get) {
        *error = utf8(file) + " is not ONNX Runtime";
        return nullptr;
    }
    const OrtApi* api = get()->GetApi(ORT_API_VERSION);
    if (!api) {
        *error = utf8(file) + " is older than ONNX Runtime 1." + std::to_string(ORT_API_VERSION) + " (version " +
                 get()->GetVersionString() + ")";
    }
    return api;
}

std::string status_message(const OrtApi* api, OrtStatus* s) {
    std::string m = api->GetErrorMessage(s);
    api->ReleaseStatus(s);
    return m;
}

const Runtime* runtime(const fs::path& cache, std::string* error) {
    std::lock_guard lock(g_runtime_mutex);
    if (g_runtime.api) return &g_runtime;
    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("ANYGBA_ONNXRUNTIME"); env && *env) {
        const fs::path p = from_utf8(env);
        candidates.push_back(fs::is_directory(p) ? p / kLibrary : p);
    }
    if (const char* base = SDL_GetBasePath()) candidates.push_back(from_utf8(base) / kLibrary);
    candidates.push_back(cache / kRuntimeFolder / kLibrary);
    std::error_code ec;
    auto found = std::find_if(candidates.begin(), candidates.end(), [&](const fs::path& p) { return fs::exists(p, ec); });
    fs::path file;
    if (found != candidates.end()) {
        file = *found;
    } else {
        const auto fetched = fetch_runtime(cache, false, error);
        if (!fetched) return nullptr;
        file = *fetched;
    }
    const OrtApi* api = open_library(file, error);
    if (!api) return nullptr;
    OrtEnv* env = nullptr;
    if (OrtStatus* s = api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "anygba", &env)) {
        *error = status_message(api, s);
        return nullptr;
    }
    g_runtime = {api, env, utf8(file)};
    return &g_runtime;
}

// The first member of OrtDmlApi (dml_provider_factory.h), whose layout is stable.
struct DmlApi {
    OrtStatus*(ORT_API_CALL* SessionOptionsAppendExecutionProvider_DML)(OrtSessionOptions* options, int device_id);
};

class OrtUpscaler final : public Upscaler {
public:
    OrtUpscaler(const Runtime& rt, const fs::path& model, int factor, Device device) : api_(rt.api), factor_(factor) {
        const std::vector<Device> order = device == Device::Auto ? std::vector{Device::DirectML, Device::Cuda, Device::Cpu}
                                                                 : std::vector{device};
        std::string errors;
        for (const Device d : order) {
            try {
                open(rt, model, d);
                device_ = device_name(d);
                break;
            } catch (const std::exception& e) {
                errors += std::string(errors.empty() ? "" : "; ") + device_name(d) + ": " + e.what();
            }
        }
        if (!session_) throw std::runtime_error(errors);
        check(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_));
        OrtAllocator* alloc = nullptr;
        check(api_->GetAllocatorWithDefaultOptions(&alloc));
        char* name = nullptr;
        check(api_->SessionGetInputName(session_, 0, alloc, &name));
        input_ = name;
        check(api_->AllocatorFree(alloc, name));
        check(api_->SessionGetOutputName(session_, 0, alloc, &name));
        output_ = name;
        check(api_->AllocatorFree(alloc, name));
        OrtTypeInfo* type = nullptr;
        check(api_->SessionGetInputTypeInfo(session_, 0, &type));
        const OrtTensorTypeAndShapeInfo* shape = nullptr;
        check(api_->CastTypeInfoToTensorInfo(type, &shape));
        std::size_t count = 0;
        check(api_->GetDimensionsCount(shape, &count));
        std::vector<std::int64_t> dims(count);
        check(api_->GetDimensions(shape, dims.data(), count));
        api_->ReleaseTypeInfo(type);
        if (count != 4) throw std::runtime_error("the model's input is not an image (N, C, H, W)");
        tile_ = dims[2] > 0 ? static_cast<int>(dims[2]) : 0;
    }

    ~OrtUpscaler() override {
        if (memory_) api_->ReleaseMemoryInfo(memory_);
        if (session_) api_->ReleaseSession(session_);
    }

    int factor() const override { return factor_; }
    const std::string& device() const override { return device_; }

    void upscale(const Image& in, Image& out) override {
        const int w = in.width, h = in.height, f = factor_;
        out.resize(w * f, h * f);
        if (w == 0 || h == 0) return;
        // Transparent pixels take an opaque neighbour's colour so edges do not darken.
        std::vector<std::uint32_t> src(in.pixels);
        bool any_clear = false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                std::uint32_t& c = src[static_cast<std::size_t>(y) * w + x];
                if ((c >> 24) >= 128) continue;
                any_clear = true;
                c = 0xFF000000u;
                for (const auto& d : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
                    const int nx = x + d.first, ny = y + d.second;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const std::uint32_t n = in.pixels[static_cast<std::size_t>(ny) * w + nx];
                    if ((n >> 24) >= 128) {
                        c = n;
                        break;
                    }
                }
            }
        }
        // Whole pictures for models of any size up to about 640x480; tiles otherwise.
        const int tile = tile_ ? tile_ : (w * h <= 640 * 480 ? 0 : 256);
        if (tile == 0) {
            run_region(src, w, h, 0, 0, w, h, out, 0, 0, w, h);
        } else {
            const int pad = std::min(8, tile / 4);
            const int step = tile - 2 * pad;
            for (int ty = 0; ty < h; ty += step) {
                for (int tx = 0; tx < w; tx += step) {
                    run_region(src, w, h, tx - pad, ty - pad, tile, tile, out, tx, ty, std::min(step, w - tx),
                               std::min(step, h - ty));
                }
            }
        }
        if (any_clear) {
            // Coverage: the alpha channel scaled bilinearly and cut at half.
            for (int y = 0; y < h * f; ++y) {
                for (int x = 0; x < w * f; ++x) {
                    const double sx = std::clamp((x + 0.5) / f - 0.5, 0.0, w - 1.0);
                    const double sy = std::clamp((y + 0.5) / f - 0.5, 0.0, h - 1.0);
                    const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
                    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
                    const double fx = sx - x0, fy = sy - y0;
                    const auto a = [&](int px, int py) { return static_cast<double>(in.pixels[static_cast<std::size_t>(py) * w + px] >> 24); };
                    const double alpha = a(x0, y0) * (1 - fx) * (1 - fy) + a(x1, y0) * fx * (1 - fy) +
                                         a(x0, y1) * (1 - fx) * fy + a(x1, y1) * fx * fy;
                    std::uint32_t& c = out.row(y)[x];
                    c = alpha >= 128 ? (c | 0xFF000000u) : (c & 0x00FFFFFFu);
                }
            }
        }
    }

private:
    void check(OrtStatus* s) const {
        if (s) throw std::runtime_error(status_message(api_, s));
    }

    void open(const Runtime& rt, const fs::path& model, Device d) {
        OrtSessionOptions* options = nullptr;
        check(api_->CreateSessionOptions(&options));
        struct Release {
            const OrtApi* api;
            OrtSessionOptions* o;
            ~Release() { api->ReleaseSessionOptions(o); }
        } release{api_, options};
        check(api_->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
        if (d == Device::DirectML) {
#ifndef _WIN32
            throw std::runtime_error("DirectML is only on Windows");
#endif
            const void* p = nullptr;
            check(api_->GetExecutionProviderApi("DML", ORT_API_VERSION, &p));
            check(api_->DisableMemPattern(options));  // DirectML requirements
            check(api_->SetSessionExecutionMode(options, ORT_SEQUENTIAL));
            check(static_cast<const DmlApi*>(p)->SessionOptionsAppendExecutionProvider_DML(options, 0));
        } else if (d == Device::Cuda) {
            OrtCUDAProviderOptionsV2* cuda = nullptr;
            check(api_->CreateCUDAProviderOptions(&cuda));
            OrtStatus* s = api_->SessionOptionsAppendExecutionProvider_CUDA_V2(options, cuda);
            api_->ReleaseCUDAProviderOptions(cuda);
            check(s);
        }
#ifdef _WIN32
        check(api_->CreateSession(rt.env, model.wstring().c_str(), options, &session_));
#else
        check(api_->CreateSession(rt.env, utf8(model).c_str(), options, &session_));
#endif
    }

    // Runs the model on the w x h region at (rx, ry) of the picture (edges repeated outside it)
    // and copies the output for picture pixels (ox, oy, ow, oh) into `out`.
    void run_region(const std::vector<std::uint32_t>& src, int w, int h, int rx, int ry, int rw, int rh, Image& out,
                    int ox, int oy, int ow, int oh) {
        const auto plane = static_cast<std::size_t>(rw) * static_cast<std::size_t>(rh);
        input_buf_.resize(plane * 3);
        for (int y = 0; y < rh; ++y) {
            const int sy = std::clamp(ry + y, 0, h - 1);
            for (int x = 0; x < rw; ++x) {
                const int sx = std::clamp(rx + x, 0, w - 1);
                const std::uint32_t c = src[static_cast<std::size_t>(sy) * w + sx];
                const std::size_t i = static_cast<std::size_t>(y) * rw + x;
                input_buf_[i] = static_cast<float>(c & 255u) / 255.0f;
                input_buf_[plane + i] = static_cast<float>((c >> 8) & 255u) / 255.0f;
                input_buf_[2 * plane + i] = static_cast<float>((c >> 16) & 255u) / 255.0f;
            }
        }
        const int f = factor_;
        const auto oplane = plane * static_cast<std::size_t>(f) * static_cast<std::size_t>(f);
        output_buf_.resize(oplane * 3);
        const std::array<std::int64_t, 4> shape{1, 3, rh, rw};
        const std::array<std::int64_t, 4> out_shape{1, 3, static_cast<std::int64_t>(rh) * f, static_cast<std::int64_t>(rw) * f};
        OrtValue* input = nullptr;
        check(api_->CreateTensorWithDataAsOrtValue(memory_, input_buf_.data(), input_buf_.size() * sizeof(float),
                                                   shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input));
        // The output goes straight into a buffer kept between frames.
        OrtValue* output = nullptr;
        OrtStatus* s = api_->CreateTensorWithDataAsOrtValue(memory_, output_buf_.data(), output_buf_.size() * sizeof(float),
                                                            out_shape.data(), out_shape.size(),
                                                            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &output);
        if (s) {
            api_->ReleaseValue(input);
            check(s);
        }
        const char* in_names[] = {input_.c_str()};
        const char* out_names[] = {output_.c_str()};
        s = api_->Run(session_, nullptr, in_names, &input, 1, out_names, 1, &output);
        api_->ReleaseValue(input);
        api_->ReleaseValue(output);
        check(s);
        const float* data = output_buf_.data();
        const int out_w = rw * f;
        const auto to_byte = [](float v) { return static_cast<std::uint32_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); };
        parallel_for(oh * f, [&](int begin, int end) {
            for (int y = oy * f + begin; y < oy * f + end; ++y) {
                const int ly = y - ry * f;
                std::uint32_t* row = out.row(y);
                for (int x = ox * f; x < (ox + ow) * f; ++x) {
                    const std::size_t i = static_cast<std::size_t>(ly) * out_w + static_cast<std::size_t>(x - rx * f);
                    row[x] = to_byte(data[i]) | (to_byte(data[oplane + i]) << 8) | (to_byte(data[2 * oplane + i]) << 16) |
                             0xFF000000u;
                }
            }
        });
    }

    const OrtApi* api_;
    OrtSession* session_ = nullptr;
    OrtMemoryInfo* memory_ = nullptr;
    std::string input_, output_, device_;
    int factor_;
    int tile_ = 0;  // fixed input size of the model, 0 when any size works
    std::vector<float> input_buf_, output_buf_;
};

}  // namespace

const ModelInfo& info(AiModel model) {
    for (const auto& m : kModels) {
        if (m.model == model) return m;
    }
    return kModels[0];
}

fs::path default_cache(const std::optional<fs::path>& data_dir) {
    if (const char* env = std::getenv("ANYGBA_AI_CACHE"); env && *env) return from_utf8(env);
    if (data_dir) return *data_dir / "ai";
    return fs::temp_directory_path() / "anygba-ai";
}

std::optional<Device> parse_device(std::string_view name) {
    if (name == "auto") return Device::Auto;
    if (name == "dml" || name == "directml") return Device::DirectML;
    if (name == "cuda") return Device::Cuda;
    if (name == "cpu") return Device::Cpu;
    return std::nullopt;
}

const char* device_name(Device d) {
    switch (d) {
    case Device::Auto: return "auto";
    case Device::DirectML: return "DirectML";
    case Device::Cuda: return "CUDA";
    case Device::Cpu: return "CPU";
    }
    return "?";
}

std::optional<fs::path> fetch_runtime(const fs::path& cache, bool directml_redistributable, std::string* error) {
#ifdef _WIN32
    const fs::path dir = cache / kRuntimeFolder;
    std::error_code ec;
    const bool have_dml = fs::exists(dir / "DirectML.dll", ec);
    if (fs::exists(dir / kLibrary, ec) && (have_dml || !directml_redistributable)) return dir / kLibrary;
    for (const Package& p : windows_runtime()) {
        // Without the redistributable, Windows' own DirectML.dll (System32, 1903 and later) is used.
        if (&p != &windows_runtime()[0] && !directml_redistributable) continue;
        const std::string url = p.url;
        const fs::path archive = cache / "downloads" / from_utf8(url.substr(url.rfind('/') + 1));
        if (auto e = download(url, p.sha256, archive)) {
            *error = *e;
            return std::nullopt;
        }
        const fs::path unpacked = cache / "downloads" / (utf8(archive.filename()) + ".d");
        if (auto e = extract(archive, unpacked, p.members)) {
            *error = *e;
            return std::nullopt;
        }
        fs::create_directories(dir, ec);
        for (const char* m : p.members) {
            // ONNX Runtime's LICENSE and DirectML's LICENSE.txt keep their own names.
            const fs::path from = unpacked / from_utf8(m);
            fs::copy_file(from, dir / from.filename(), fs::copy_options::overwrite_existing, ec);
            if (ec) {
                *error = "cannot copy " + utf8(from) + ": " + ec.message();
                return std::nullopt;
            }
        }
    }
    return dir / kLibrary;
#else
    (void)cache;
    (void)directml_redistributable;
    *error = std::string("no ONNX Runtime found: set ANYGBA_ONNXRUNTIME to ") + kLibrary +
             " from an ONNX Runtime 1.24+ release, or put it next to the program";
    return std::nullopt;
#endif
}

std::optional<fs::path> ensure_model(AiModel model, const fs::path& cache, std::string* error) {
    const ModelInfo& m = info(model);
    const fs::path dir = cache / "models" / m.name;
    const fs::path onnx = dir / from_utf8(m.onnx);
    fs::path dynamic = onnx;
    dynamic.replace_extension(".dynamic.onnx");
    std::error_code ec;
    if (fs::exists(dynamic, ec)) return dynamic;
    const std::string url = m.url;
    const fs::path archive = dir / from_utf8(url.substr(url.rfind('/') + 1));
    if (auto e = download(url, m.sha256, archive)) {
        *error = *e;
        return std::nullopt;
    }
    if (auto e = extract(archive, dir, {})) {
        *error = "cannot unpack " + utf8(archive) + ": " + *e;
        return std::nullopt;
    }
    const auto bytes = read_file(onnx);
    if (!bytes) {
        *error = utf8(onnx) + " is missing from " + utf8(archive);
        return std::nullopt;
    }
    const auto patched = make_size_dynamic(*bytes, error);
    if (!patched) return std::nullopt;
    if (!write_file(dynamic, *patched)) {
        *error = "cannot write " + utf8(dynamic);
        return std::nullopt;
    }
    return dynamic;
}

std::unique_ptr<Upscaler> Upscaler::create(AiModel model, const fs::path& cache, Device device, std::string* error) {
    std::string local;
    std::string* err = error ? error : &local;
    const Runtime* rt = runtime(cache, err);
    if (!rt) return nullptr;
    const auto path = ensure_model(model, cache, err);
    if (!path) return nullptr;
    try {
        return std::make_unique<OrtUpscaler>(*rt, *path, info(model).factor, device);
    } catch (const std::exception& e) {
        *err = std::string(info(model).name) + ": " + e.what();
        return nullptr;
    }
}

}  // namespace anygba::video::hd::ai
