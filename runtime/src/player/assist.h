#pragma once

#include "player/touch.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// The floating menu button on Android (docs/user/ANDROID.md): a round button that can be
// dragged anywhere and rests against the nearest screen edge, and the quick menu it opens.
// Geometry and the gesture state machine only; touch_overlay.cpp draws them. Times are in
// nanoseconds from any monotonic clock.
namespace anygba::player::assist {

using anyport::platform::FRect;
using touch::Edge;

struct Point {
    float x = 0, y = 0;
};

// Diameter of the button for an output of width x height pixels.
float diameter(float width, float height);
// Centre of a button resting at `edge`/`pos` (permille along it) inside `safe`.
Point resting_centre(Edge edge, int pos, FRect safe, float diameter);
// The edge nearest to (x, y) and the permille position along it, for a button released there.
std::pair<Edge, int> snap(float x, float y, FRect safe, float diameter);

// Quick menu entries, in the order they are shown.
enum class Item : std::uint8_t {
    SaveState, LoadState, FastForward, FastForwardSpeed, Cheats, Screenshot, Filter, Widescreen,
    EditControls, ShowControls, GameMenu, Settings, Exit, Count
};
inline constexpr int kItems = static_cast<int>(Item::Count);
inline constexpr int kSlots = 9;

// A card of `count` equal cells centred on the screen (columns chosen for the orientation).
struct Grid {
    FRect card;
    std::vector<FRect> cells;
    float cell = 0;
};
Grid grid(int count, float width, float height, FRect safe);
// Index of the cell at (x, y), or -1.
int cell_at(const Grid& grid, float x, float y);

// The layout editor's toolbar: Smaller, Bigger, Reset, Done in a row in the middle of the screen.
enum class Tool : std::uint8_t { Smaller, Bigger, Reset, Done, Count };
inline constexpr int kTools = static_cast<int>(Tool::Count);
std::array<FRect, kTools> toolbar(float width, float height, FRect safe);
std::optional<Tool> tool_at(const std::array<FRect, kTools>& bar, float x, float y);

// Tap, long press and drag of the floating button.
class Button {
public:
    enum class Gesture : std::uint8_t { None, Tap, LongPress, Dropped };

    static constexpr std::uint64_t kLongPressNs = 600'000'000;
    static constexpr std::uint64_t kIdleNs = 3'000'000'000;
    static constexpr std::uint64_t kFadeNs = 400'000'000;
    static constexpr std::uint64_t kSnapNs = 180'000'000;

    // Places the button for this output; keeps a drag or snap animation going.
    void place(Edge edge, int pos, FRect safe, float diameter);

    // A finger went down at (x, y). True when it landed on the button, which then owns it.
    bool down(std::int64_t finger, float x, float y, std::uint64_t now);
    void move(std::int64_t finger, float x, float y, std::uint64_t now);
    // The owning finger lifted (or was cancelled): a tap, the end of a drag (the new resting
    // place is in dropped()), or nothing after a long press.
    Gesture up(std::int64_t finger, std::uint64_t now, bool cancelled = false);
    // LongPress once the finger has stayed still long enough; None otherwise.
    Gesture tick(std::uint64_t now);

    bool owns(std::int64_t finger) const { return finger_ && *finger_ == finger; }
    bool held() const { return finger_.has_value(); }
    std::pair<Edge, int> dropped() const { return dropped_; }
    // Where to draw it now (follows the finger, then slides to the edge).
    Point centre(std::uint64_t now) const;
    float diameter() const { return diameter_; }
    // 1 while in use, fading to a dimmer level after kIdleNs without a touch.
    float activity(std::uint64_t now) const;
    void wake(std::uint64_t now) { last_used_ns_ = now; }

private:
    Point rest_{};
    float diameter_ = 0;
    FRect safe_{};
    std::optional<std::int64_t> finger_;
    Point down_at_{}, finger_at_{}, grab_{};
    std::uint64_t down_ns_ = 0;
    bool dragging_ = false;
    bool long_fired_ = false;
    std::pair<Edge, int> dropped_{Edge::Right, 0};
    Point snap_from_{};
    std::uint64_t snap_start_ns_ = 0;
    bool snapping_ = false;
    std::uint64_t last_used_ns_ = 0;
};

const char* item_label(Item item);

}  // namespace anygba::player::assist
