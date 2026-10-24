#include "video/hd/onnx_patch.h"

#include <functional>
#include <span>

namespace anygba::video::hd::ai {

namespace {

using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;

// Protobuf field numbers (onnx.proto3).
constexpr int kModelGraph = 7;
constexpr int kGraphInput = 11, kGraphOutput = 12, kGraphValueInfo = 13;
constexpr int kValueInfoType = 2;
constexpr int kTypeTensor = 1;
constexpr int kTensorShape = 2;
constexpr int kShapeDim = 1;
constexpr int kDimValue = 1, kDimParam = 2;

void put_varint(Bytes& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_len(Bytes& out, int field, View payload) {
    put_varint(out, (static_cast<std::uint64_t>(field) << 3) | 2);
    put_varint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

bool get_varint(View in, std::size_t& i, std::uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (i >= in.size()) return false;
        const std::uint8_t b = in[i++];
        v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

// Copies a message field by field; `edit` may replace a length-delimited field (returns true
// when it wrote the replacement, or wants the field dropped).
bool rewrite(View in, Bytes& out, const std::function<bool(int field, View payload, Bytes& out)>& edit) {
    std::size_t i = 0;
    while (i < in.size()) {
        const std::size_t start = i;
        std::uint64_t tag;
        if (!get_varint(in, i, tag)) return false;
        const int field = static_cast<int>(tag >> 3);
        const int wire = static_cast<int>(tag & 7);
        std::size_t end;
        switch (wire) {
        case 0: {
            std::uint64_t ignored;
            if (!get_varint(in, i, ignored)) return false;
            end = i;
            break;
        }
        case 1: end = i + 8; break;
        case 5: end = i + 4; break;
        case 2: {
            std::uint64_t len;
            if (!get_varint(in, i, len) || len > in.size() - i) return false;
            end = i + static_cast<std::size_t>(len);
            if (edit(field, in.subspan(i, static_cast<std::size_t>(len)), out)) {
                i = end;
                continue;
            }
            break;
        }
        default: return false;  // groups are not used by ONNX
        }
        if (end > in.size()) return false;
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(start), in.begin() + static_cast<std::ptrdiff_t>(end));
        i = end;
    }
    return true;
}

bool nested(View payload, Bytes& out, int field, const std::function<bool(int, View, Bytes&)>& edit) {
    Bytes sub;
    if (!rewrite(payload, sub, edit)) return false;
    put_len(out, field, sub);
    return true;
}

}  // namespace

std::optional<Bytes> make_size_dynamic(const Bytes& model, std::string* error) {
    bool ok = true;
    int dims_changed = 0;
    const auto shape = [&](View payload, Bytes& out) {
        int index = 0;
        return nested(payload, out, kTensorShape, [&](int f, View, Bytes& o) {
            if (f != kShapeDim) return false;
            const int d = index++;
            if (d != 2 && d != 3) return false;
            Bytes dim;
            const std::string name = d == 2 ? "height" : "width";
            put_len(dim, kDimParam, View(reinterpret_cast<const std::uint8_t*>(name.data()), name.size()));
            put_len(o, kShapeDim, dim);
            ++dims_changed;
            return true;
        });
    };
    const auto value_info = [&](View payload, Bytes& out, int field) {
        return nested(payload, out, field, [&](int f, View p, Bytes& o) {
            if (f != kValueInfoType) return false;
            ok = ok && nested(p, o, kValueInfoType, [&](int f2, View p2, Bytes& o2) {
                if (f2 != kTypeTensor) return false;
                ok = ok && nested(p2, o2, kTypeTensor, [&](int f3, View p3, Bytes& o3) {
                    if (f3 != kTensorShape) return false;
                    ok = ok && shape(p3, o3);
                    return true;
                });
                return true;
            });
            return true;
        });
    };
    Bytes out;
    const bool parsed = rewrite(model, out, [&](int f, View p, Bytes& o) {
        if (f != kModelGraph) return false;
        ok = ok && nested(p, o, kModelGraph, [&](int g, View gp, Bytes& go) {
            if (g == kGraphValueInfo) return true;  // drop: the fixed intermediate shapes
            if (g != kGraphInput && g != kGraphOutput) return false;
            ok = ok && value_info(gp, go, g);
            return true;
        });
        return true;
    });
    if (!parsed || !ok || dims_changed == 0) {
        if (error) *error = "not an ONNX model with image inputs (N, C, H, W)";
        return std::nullopt;
    }
    return out;
}

}  // namespace anygba::video::hd::ai
