#include "apu/apu.h"

// DirectSound A/B (GBATEK "GBA Sound Channel A and B"): 32-byte FIFOs of signed 8-bit
// samples. The selected timer's overflow plays the next byte; once 16 bytes or fewer are
// left, the sound DMA (channel 1 or 2 in FIFO mode) is asked for 16 more.
namespace anygba {

void Apu::Fifo::push(std::uint8_t b) {
    fed = true;
    if (size < data.size()) {
        data[(read + size) % data.size()] = static_cast<std::int8_t>(b);
        ++size;
    }
}

void Apu::timer_overflow(int timer, std::uint64_t at) {
    if (!(reg(0x084) & 0x80)) {
        return;
    }
    catch_up(at);
    const std::uint16_t cnt = reg(0x082);
    for (int f = 0; f < 2; ++f) {
        const int selected = (cnt >> (f == 0 ? 10 : 14)) & 1;
        if (selected != timer) {
            continue;
        }
        Fifo& fifo = fifo_[static_cast<std::size_t>(f)];
        if (fifo.size > 0) {
            fifo.current = fifo.data[fifo.read];
            fifo.read = (fifo.read + 1) % static_cast<unsigned>(fifo.data.size());
            --fifo.size;
        } else if (fifo.fed) {
            ++underruns_;
        }
        if (fifo.size <= 16) {
            sys_.request_fifo_dma(f);
        }
    }
}

}  // namespace anygba
