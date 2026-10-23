#include "anygba/system.hpp"
#include "support/corpus.h"
#include "support/headless_run.h"
#include "support/png.h"
#include "support/test_roms.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <vector>

using anygba::System;

namespace {

const std::filesystem::path kData = std::filesystem::path(ANYGBA_TEST_DATA_DIR) / "apu";
constexpr std::size_t kWindow = 1024;
constexpr std::size_t kBlock = 512;
constexpr long kMaxLag = 512;

struct Stereo {
    std::vector<double> l, r;
};

Stereo split(const std::vector<std::int16_t>& interleaved) {
    Stereo s;
    for (std::size_t i = 0; i + 1 < interleaved.size(); i += 2) {
        s.l.push_back(interleaved[i]);
        s.r.push_back(interleaved[i + 1]);
    }
    return s;
}

// First frame where either channel moves more than 300 from the first frame (as in
// tests/oracle/make-apu-references.py).
std::size_t onset(const Stereo& s) {
    for (std::size_t i = 0; i < s.l.size(); ++i) {
        if (std::abs(s.l[i] - s.l[0]) > 300 || std::abs(s.r[i] - s.r[0]) > 300) return i;
    }
    return SIZE_MAX;
}

// Pearson correlation; NaN when either side is constant.
double pearson(const double* a, const double* b, std::size_t n) {
    double ma = 0, mb = 0;
    for (std::size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);
    double sab = 0, saa = 0, sbb = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return (saa == 0 || sbb == 0) ? std::nan("") : sab / std::sqrt(saa * sbb);
}

std::vector<std::int16_t> capture(const std::vector<std::uint8_t>& rom, std::uint64_t frames,
                                  const std::vector<anygba::test::KeyHold>& keys) {
    std::vector<std::int16_t> sound;
    anygba::test::run_headless(rom, frames, keys, [&sound](std::uint64_t, System& s) { s.drain_audio(sound); });
    return sound;
}

struct Case {
    std::string name, check;
    std::size_t start = 0, count = 0;
    double threshold = 0;
    std::string corpus_rom;  // empty: a self-authored test ROM
    std::uint64_t frames = 62;
    std::vector<anygba::test::KeyHold> keys;
};

struct Outcome {
    double score = 0;  // corr: mean correlation; track: worst period-count difference
    bool ok = false;
    std::string detail;
};

Outcome correlate(const Stereo& ours, std::size_t ours_onset, const Stereo& ref, const Case& c) {
    double sum = 0;
    for (std::size_t w = 0; w < c.count; ++w) {
        const std::size_t r0 = w * kWindow;
        const auto base = static_cast<long>(ours_onset + c.start + r0);
        double best = -2;
        for (long lag = -kMaxLag; lag <= kMaxLag; ++lag) {
            const long o0 = base + lag;
            if (o0 < 0 || static_cast<std::size_t>(o0) + kWindow > ours.l.size()) continue;
            double total = 0;
            int channels = 0;
            for (const auto [mine, theirs] : {std::pair{&ours.l, &ref.l}, std::pair{&ours.r, &ref.r}}) {
                const double v = pearson(mine->data() + o0, theirs->data() + r0, kWindow);
                if (!std::isnan(v)) {
                    total += v;
                    ++channels;
                }
            }
            if (channels && total / channels > best) best = total / channels;
        }
        sum += best;
    }
    Outcome out;
    out.score = sum / static_cast<double>(c.count);
    out.ok = out.score >= c.threshold;
    out.detail = "mean windowed correlation " + std::to_string(out.score);
    return out;
}

// Rising crossings of the block mean, and whether the block has any signal at all.
std::pair<int, bool> periods(const std::vector<double>& x, std::size_t from) {
    double m = 0, lo = 1e9, hi = -1e9;
    for (std::size_t i = from; i < from + kBlock; ++i) {
        m += x[i];
        lo = std::min(lo, x[i]);
        hi = std::max(hi, x[i]);
    }
    m /= kBlock;
    int ups = 0;
    for (std::size_t i = from + 1; i < from + kBlock; ++i) {
        ups += x[i - 1] < m && x[i] >= m;
    }
    return {ups, hi - lo > 600};
}

Outcome track(const Stereo& ours, std::size_t ours_onset, const Stereo& ref, const Case& c) {
    Outcome out;
    int worst = 0;
    long ours_silent = -1, ref_silent = -1;
    for (std::size_t b = 0; b < c.count; ++b) {
        const auto [mine, mine_on] = periods(ours.l, ours_onset + c.start + b * kBlock);
        const auto [theirs, theirs_on] = periods(ref.l, b * kBlock);
        if (!mine_on && ours_silent < 0) ours_silent = static_cast<long>(b);
        if (!theirs_on && ref_silent < 0) ref_silent = static_cast<long>(b);
        if (mine_on && theirs_on) worst = std::max(worst, std::abs(mine - theirs));
    }
    out.score = worst;
    out.ok = worst <= c.threshold &&
             (ref_silent < 0 ? ours_silent < 0 : ours_silent >= 0 && std::abs(ours_silent - ref_silent) <= 1);
    out.detail = "worst period-count difference " + std::to_string(worst) + ", silent from block " +
                 std::to_string(ours_silent) + " (mGBA " + std::to_string(ref_silent) + ")";
    return out;
}

Outcome run_case(const Case& c, std::vector<std::uint8_t> rom) {
    const auto packed = anygba::test::read_file(kData / (c.name + ".pcmz"));
    const auto bytes = anygba::test::inflate_zlib(packed);
    std::vector<std::int16_t> ref_pcm(bytes.size() / 2);
    std::memcpy(ref_pcm.data(), bytes.data(), ref_pcm.size() * 2);
    const Stereo ref = split(ref_pcm);
    const Stereo ours = split(capture(rom, c.frames, c.keys));
    const std::size_t o = onset(ours);
    if (o == SIZE_MAX) return {0, false, "AnyGBA produced no sound"};
    return c.check == "corr" ? correlate(ours, o, ref, c) : track(ours, o, ref, c);
}

}  // namespace

TEST_CASE("APU output follows mGBA on self-authored PSG and DirectSound test ROMs") {
    std::ifstream f(kData / "cases.txt");
    REQUIRE(f.good());
    std::vector<Case> cases;
    for (std::string line; std::getline(f, line);) {
        std::istringstream in(line.substr(0, line.find('#')));
        Case c;
        if (!(in >> c.name >> c.start >> c.count >> c.check >> c.threshold)) continue;
        for (std::string kv; in >> kv;) {
            const auto eq = kv.find('=');
            const std::string key = kv.substr(0, eq), value = kv.substr(eq + 1);
            if (key == "rom") {
                c.corpus_rom = value;
            } else if (key == "frames") {
                c.frames = std::stoull(value);
            } else if (key == "keys") {
                std::istringstream holds(value);
                for (std::string h; std::getline(holds, h, ',');) c.keys.push_back(anygba::test::parse_key_hold(h));
            } else {
                FAIL("unknown option " << kv << " in apu/cases.txt");
            }
        }
        cases.push_back(c);
    }
    REQUIRE(cases.size() == 10);
    const auto roms = anygba::test::apu_test_roms();
    std::vector<std::future<Outcome>> runs;
    std::vector<const Case*> ran;
    for (const Case& c : cases) {
        std::vector<std::uint8_t> rom;
        if (!c.corpus_rom.empty()) {
            auto data = anygba::test::read_corpus_file(c.corpus_rom);
            if (!data) continue;
            rom = std::move(*data);
        } else {
            const auto it = std::find_if(roms.begin(), roms.end(), [&c](const auto& r) { return r.name == c.name; });
            REQUIRE(it != roms.end());
            rom = it->rom;
        }
        ran.push_back(&c);
        runs.push_back(std::async(std::launch::async, run_case, c, std::move(rom)));
    }
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const Outcome o = runs[i].get();
        CAPTURE(ran[i]->name);
        MESSAGE(ran[i]->name << ": " << o.detail);
        CHECK(o.ok);
    }
}

namespace {

anygba::Options quiet() {
    anygba::Options o;
    o.log_misses = false;
    return o;
}

std::vector<std::uint8_t> idle_rom() {
    std::vector<std::uint8_t> rom(0x200, 0);
    const std::uint32_t idle = 0xEAFFFFFE;
    std::memcpy(rom.data(), &idle, 4);
    return rom;
}

void run_frames(System& sys, std::uint64_t frames) {
    const std::uint64_t until = sys.frames() + frames;
    sys.on_vblank = [until](System& s) {
        if (s.frames() >= until) s.stop();
    };
    sys.run();
}

}  // namespace

TEST_CASE("APU silence, length counter, sweep overflow and master disable") {
    System sys(idle_rom(), nullptr, quiet());
    run_frames(sys, 2);
    std::vector<std::int16_t> sound;
    sys.drain_audio(sound);
    CHECK(sound.size() > 2 * 900);  // first VBlank at line 160, then one full frame
    CHECK(std::all_of(sound.begin(), sound.end(), [](std::int16_t s) { return s == 0; }));  // bias 0x200 = centre

    sys.io_write16(0x084, 0x0080);
    sys.io_write16(0x080, 0x2277);
    sys.io_write16(0x068, 0xF03F);           // square 2: volume 15, length 64-63 = 1/256 s
    sys.io_write16(0x06C, 0xC000 | 1750);    // trigger with the length flag
    CHECK((sys.io_read16(0x084) & 2) != 0);
    run_frames(sys, 1);                      // 1/60 s > 1/256 s
    CHECK((sys.io_read16(0x084) & 2) == 0);

    sys.io_write16(0x060, 0x0011);           // sweep up, shift 1, time 1/128 s
    sys.io_write16(0x062, 0xF080);
    sys.io_write16(0x064, 0x8000 | 1200);    // 1200 -> 1800 at the first step, whose next step would pass 2047
    CHECK((sys.io_read16(0x084) & 1) != 0);
    run_frames(sys, 2);
    CHECK((sys.io_read16(0x084) & 1) == 0);

    sys.io_write16(0x078, 0xF000);
    sys.io_write16(0x07C, 0x8000);
    CHECK((sys.io_read16(0x084) & 8) != 0);
    sys.io_write16(0x084, 0x0000);           // master off: PSG registers cleared, channels stop
    CHECK(sys.io_read16(0x084) == 0);
    CHECK(sys.io_read16(0x078) == 0);
    sys.io_write16(0x080, 0x1234);           // ignored while off
    CHECK(sys.io_read16(0x080) == 0);
}

TEST_CASE("DirectSound FIFO plays one byte per timer overflow and refills by DMA") {
    System sys(idle_rom(), nullptr, quiet());
    for (std::uint32_t i = 0; i < 64; ++i) {
        sys.write8(0x02000000 + i, i * 2);
    }
    sys.io_write16(0x084, 0x0080);
    sys.io_write16(0x082, 0x0B04);           // FIFO A: 100%, L+R, timer 0, reset
    sys.write32(0x040000BC, 0x02000000);
    sys.write32(0x040000C0, 0x040000A0);
    sys.write16(0x040000C6, 0xB600);         // DMA 1: enabled, sound FIFO timing, repeat, 32-bit
    sys.io_write16(0x100, 0xFE00);           // 512 cycles per byte = one byte per output sample
    sys.io_write16(0x102, 0x0080);
    run_frames(sys, 1);
    std::vector<std::int16_t> sound;
    sys.drain_audio(sound);
    // The FIFO started empty: the first overflow requested 16 bytes, after which bytes 0, 2,
    // 4, ... (scaled by 4 * 64 per step) come out once per sample.
    std::vector<int> steps;
    for (std::size_t i = 2; i + 2 < 2 * 40; i += 2) {
        steps.push_back((sound[i + 2] - sound[i]) / 64);
    }
    CHECK(std::count(steps.begin(), steps.end(), 8) >= 30);
}
