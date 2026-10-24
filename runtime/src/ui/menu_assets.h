#pragma once

#include "ui/anyui/font.hpp"
#include "ui/anyui/menu_view.hpp"

#include <span>

// The menu's built-in font (Noto Sans, SIL Open Font License, runtime/src/ui/fonts/) and
// its look options from the player settings.
namespace anygba::ui {

const anyui::Font& regular_font();
const anyui::Font& bold_font();

}  // namespace anygba::ui
