#include "player/assist.h"

#include <algorithm>
#include <cmath>

namespace anygba::player::assist {

namespace {

float margin(float diameter) { return diameter * 0.15f; }

float distance(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

bool inside(const FRect& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

// Range the button centre can take along one axis of the safe area.
std::pair<float, float> span(float start, float length, float diameter) {
    const float lo = start + margin(diameter) + diameter / 2;
    const float hi = start + length - margin(diameter) - diameter / 2;
    return {lo, std::max(lo, hi)};
}

}  // namespace

float diameter(float width, float height) { return std::clamp(std::min(width, height) * 0.11f, 24.0f, 220.0f); }

Point resting_centre(Edge edge, int pos, FRect safe, float d) {
    const auto [x0, x1] = span(safe.x, safe.w, d);
    const auto [y0, y1] = span(safe.y, safe.h, d);
    const float t = static_cast<float>(std::clamp(pos, 0, 1000)) / 1000.0f;
    switch (edge) {
    case Edge::Left: return {x0, y0 + t * (y1 - y0)};
    case Edge::Right: return {x1, y0 + t * (y1 - y0)};
    case Edge::Top: return {x0 + t * (x1 - x0), y0};
    case Edge::Bottom: return {x0 + t * (x1 - x0), y1};
    }
    return {x1, y0};
}

std::pair<Edge, int> snap(float x, float y, FRect safe, float d) {
    const float left = x - safe.x, right = safe.x + safe.w - x, top = y - safe.y, bottom = safe.y + safe.h - y;
    const float nearest = std::min({left, right, top, bottom});
    const Edge edge = nearest == left ? Edge::Left : nearest == right ? Edge::Right : nearest == top ? Edge::Top : Edge::Bottom;
    const bool vertical = edge == Edge::Left || edge == Edge::Right;
    const auto [lo, hi] = vertical ? span(safe.y, safe.h, d) : span(safe.x, safe.w, d);
    const float along = vertical ? y : x;
    const float t = hi > lo ? std::clamp((along - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
    return {edge, static_cast<int>(std::lround(t * 1000))};
}

Grid grid(int count, float width, float height, FRect safe) {
    if (safe.w <= 0 || safe.h <= 0) safe = {0, 0, width, height};
    Grid g;
    if (count <= 0) return g;
    const int cols = std::min(count, width >= height ? 5 : 4);
    const int rows = (count + cols - 1) / cols;
    const float pad_k = 0.18f;
    const float cell = std::min({safe.w * 0.92f / (static_cast<float>(cols) + 2 * pad_k),
                                 safe.h * 0.88f / (static_cast<float>(rows) + 2 * pad_k), std::min(width, height) * 0.24f});
    const float pad = cell * pad_k;
    const float cw = static_cast<float>(cols) * cell + 2 * pad, ch = static_cast<float>(rows) * cell + 2 * pad;
    g.card = {safe.x + (safe.w - cw) / 2, safe.y + (safe.h - ch) / 2, cw, ch};
    g.cell = cell;
    for (int i = 0; i < count; ++i) {
        const int row = i / cols, col = i % cols;
        const int in_row = std::min(cols, count - row * cols);
        const float row_left = g.card.x + pad + static_cast<float>(cols - in_row) * cell / 2;
        g.cells.push_back({row_left + static_cast<float>(col) * cell, g.card.y + pad + static_cast<float>(row) * cell, cell, cell});
    }
    return g;
}

int cell_at(const Grid& g, float x, float y) {
    for (std::size_t i = 0; i < g.cells.size(); ++i) {
        if (inside(g.cells[i], x, y)) return static_cast<int>(i);
    }
    return -1;
}

std::array<FRect, kTools> toolbar(float width, float height, FRect safe) {
    if (safe.w <= 0 || safe.h <= 0) safe = {0, 0, width, height};
    const float h = std::min(width, height) * 0.09f;
    const float gap = h * 0.3f;
    const float w = std::min(h * 2.6f, (safe.w * 0.9f - gap * (kTools - 1)) / kTools);
    const float total = w * kTools + gap * (kTools - 1);
    const float x0 = safe.x + (safe.w - total) / 2, y = safe.y + (safe.h - h) / 2;
    std::array<FRect, kTools> bar{};
    for (int i = 0; i < kTools; ++i) bar[static_cast<std::size_t>(i)] = {x0 + static_cast<float>(i) * (w + gap), y, w, h};
    return bar;
}

std::optional<Tool> tool_at(const std::array<FRect, kTools>& bar, float x, float y) {
    for (int i = 0; i < kTools; ++i) {
        if (inside(bar[static_cast<std::size_t>(i)], x, y)) return static_cast<Tool>(i);
    }
    return std::nullopt;
}

void Button::place(Edge edge, int pos, FRect safe, float d) {
    diameter_ = d;
    safe_ = safe;
    rest_ = resting_centre(edge, pos, safe, d);
    if (!finger_) dropped_ = {edge, pos};
}

bool Button::down(std::int64_t finger, float x, float y, std::uint64_t now) {
    if (finger_ || diameter_ <= 0 || distance({x, y}, centre(now)) > diameter_ * 0.6f) return false;
    const Point c = centre(now);
    finger_ = finger;
    down_at_ = finger_at_ = {x, y};
    grab_ = {c.x - x, c.y - y};
    down_ns_ = now;
    dragging_ = false;
    long_fired_ = false;
    snapping_ = false;
    wake(now);
    return true;
}

void Button::move(std::int64_t finger, float x, float y, std::uint64_t now) {
    if (!owns(finger)) return;
    finger_at_ = {x, y};
    if (!dragging_ && !long_fired_ && distance(down_at_, finger_at_) > diameter_ * 0.25f) dragging_ = true;
    wake(now);
}

Button::Gesture Button::up(std::int64_t finger, std::uint64_t now, bool cancelled) {
    if (!owns(finger)) return Gesture::None;
    const Point c = centre(now);
    finger_.reset();
    wake(now);
    if (dragging_) {
        dragging_ = false;
        dropped_ = snap(c.x, c.y, safe_, diameter_);
        rest_ = resting_centre(dropped_.first, dropped_.second, safe_, diameter_);
        snap_from_ = c;
        snap_start_ns_ = now;
        snapping_ = true;
        return Gesture::Dropped;
    }
    if (long_fired_ || cancelled) return Gesture::None;
    return Gesture::Tap;
}

Button::Gesture Button::tick(std::uint64_t now) {
    if (!finger_ || dragging_ || long_fired_ || now - down_ns_ < kLongPressNs) return Gesture::None;
    long_fired_ = true;
    return Gesture::LongPress;
}

Point Button::centre(std::uint64_t now) const {
    if (finger_ && dragging_) {
        const float r = diameter_ / 2;
        return {std::clamp(finger_at_.x + grab_.x, safe_.x + r, std::max(safe_.x + r, safe_.x + safe_.w - r)),
                std::clamp(finger_at_.y + grab_.y, safe_.y + r, std::max(safe_.y + r, safe_.y + safe_.h - r))};
    }
    if (snapping_ && now >= snap_start_ns_ && now - snap_start_ns_ < kSnapNs) {
        const float t = static_cast<float>(now - snap_start_ns_) / static_cast<float>(kSnapNs);
        const float e = 1 - (1 - t) * (1 - t);  // ease out
        return {snap_from_.x + (rest_.x - snap_from_.x) * e, snap_from_.y + (rest_.y - snap_from_.y) * e};
    }
    return rest_;
}

float Button::activity(std::uint64_t now) const {
    if (finger_ || now < last_used_ns_) return 1;
    const std::uint64_t idle = now - last_used_ns_;
    if (idle < kIdleNs) return 1;
    if (idle >= kIdleNs + kFadeNs) return 0;
    return 1 - static_cast<float>(idle - kIdleNs) / static_cast<float>(kFadeNs);
}

const char* item_label(Item item) {
    switch (item) {
    case Item::SaveState: return "Save";
    case Item::LoadState: return "Load";
    case Item::FastForward: return "Fast";
    case Item::FastForwardSpeed: return "Speed";
    case Item::Cheats: return "Cheats";
    case Item::Screenshot: return "Shot";
    case Item::Filter: return "Filter";
    case Item::Widescreen: return "Wide";
    case Item::EditControls: return "Layout";
    case Item::ShowControls: return "Pad";
    case Item::GameMenu: return "Menu";
    case Item::Settings: return "Setup";
    case Item::Exit: return "Exit";
    case Item::Count: break;
    }
    return "";
}

}  // namespace anygba::player::assist
