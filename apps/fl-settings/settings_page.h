// fl-settings page interface
//
// A page is one .cpp file in pages/ that defines
//     fld::PageInfo page_<id>();
// and is listed in CMakeLists.txt with fld_settings_page(<id> pages/<file>.cpp).
// Pages apply their changes right away unless they say otherwise.
#pragma once
#include <FL/Fl_Group.H>
#include <string>
#include <vector>

namespace fld {

struct PageInfo {
  std::string id, title, icon, category, keywords;
  // creates the page inside x,y,w,h (called when the page is first shown)
  Fl_Group *(*create)(int x, int y, int w, int h);
};

// all compiled-in pages, in sidebar order (generated: pages_registry.cpp)
const std::vector<PageInfo> &all_pages();

} // namespace fld
