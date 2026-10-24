#pragma once

#include "control/json.h"

#include <string>
#include <vector>

// The MCP tools anygba-mcp offers (docs/user/AI-CONTROL.md). Most forward their arguments to
// the game method of the same name (docs/dev/control-protocol.md); `local` tools run in
// anygba-mcp itself.
namespace anygba::mcp {

struct Tool {
    std::string name;
    std::string description;
    control::Json input_schema;
    bool local = false;
    bool mutating = false;  // annotations: readOnlyHint false
};

const std::vector<Tool>& tools();
const Tool* find_tool(const std::string& name);

}  // namespace anygba::mcp
