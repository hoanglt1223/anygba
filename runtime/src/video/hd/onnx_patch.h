#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Rewrites an ONNX model (protobuf) so its image inputs and outputs take any height and width:
// dimensions 2 and 3 of every graph input and output become the symbolic "height"/"width", and
// the recorded intermediate shapes (graph value_info) are dropped. Upscalers published with a
// fixed tile size (1x3x128x128) then run on a whole frame at once. Returns nullopt with `error`
// set for data that is not an ONNX model.
namespace anygba::video::hd::ai {

std::optional<std::vector<std::uint8_t>> make_size_dynamic(const std::vector<std::uint8_t>& model, std::string* error);

}  // namespace anygba::video::hd::ai
