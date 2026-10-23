#include <anyport/platform/audio.h>
#include <anyport/platform/context.h>

#include <doctest.h>

#include <cstdint>
#include <vector>

namespace pf = anyport::platform;

TEST_CASE("audio stream rejects invalid configuration") {
    pf::Context context({.headless = true});
    CHECK_THROWS_AS((pf::AudioStream{{.sample_rate = 0, .channels = 2}}), pf::Error);
    CHECK_THROWS_AS((pf::AudioStream{{.sample_rate = 32768, .channels = 0}}), pf::Error);
    CHECK_THROWS_AS((pf::AudioStream{{.sample_rate = 32768, .channels = 9}}), pf::Error);
}

TEST_CASE("audio stream queues whole frames only") {
    pf::Context context({.headless = true});
    pf::AudioStream stream({.sample_rate = 32768, .channels = 2});
    CHECK(stream.sample_rate() == 32768);
    CHECK(stream.channels() == 2);

    const std::vector<std::int16_t> odd(3);
    CHECK_THROWS_AS(stream.push(odd), pf::Error);

    stream.push({});
    const std::vector<std::int16_t> samples(2 * 4096);
    stream.push(samples);
    const auto queued = stream.queued_frames();
    CHECK(queued > 0);
    CHECK(queued <= 4096);
}
