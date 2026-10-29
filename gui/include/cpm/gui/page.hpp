#pragma once

#include <string_view>

namespace cpm::gui {

// gui/web/index.html, embedded at build time by cmake/EmbedFile.cmake
// (GUI-Q8): no runtime asset path to get wrong, and no file-serving code path
// to review for traversal. The server serves exactly this one document.
std::string_view page();

}  // namespace cpm::gui
