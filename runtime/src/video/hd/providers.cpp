#include "video/hd/widescreen.h"

namespace anygba::video::hd {

std::unique_ptr<MapProvider> make_pokeemerald_field(const std::map<std::string, std::uint32_t>& settings,
                                                    std::string* error);

std::unique_ptr<MapProvider> make_provider(std::string_view name, const std::map<std::string, std::uint32_t>& settings,
                                           std::string* error) {
    if (name == "pokeemerald-field") return make_pokeemerald_field(settings, error);
    if (error) *error = "unknown widescreen provider '" + std::string(name) + "' (pokeemerald-field)";
    return nullptr;
}

}  // namespace anygba::video::hd
