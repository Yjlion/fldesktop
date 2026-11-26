// fl-settings page interface
//
// A page is one .cpp file in pages/ that defines
//     fld::PageInfo page_<id>();
// and is listed in CMakeLists.txt with fld_settings_page(<id> pages/<file>.cpp).
// Pages apply their changes right away unless they say otherwise.
#pragma once
#include <FL/Fl_Group.H>
#include <string>
#include <utility>
#include <vector>

namespace fld {

struct PageInfo {
  std::string id, title, icon, category, keywords;
  // creates the page inside x,y,w,h (called when the page is first shown)
  Fl_Group *(*create)(int x, int y, int w, int h);
  // when that page is in the build, this one is one of its tabs (labelled
  // with this page's title) instead of a page of its own
  std::string merged_into = {};
};

// the page to show for id: the page it is merged into when that one is in
// the build; nullptr when the build has neither
const PageInfo *resolve_page(const std::string &id);
// false for pages shown as a tab of another page
bool page_in_sidebar(const PageInfo &p);

// all compiled-in pages, in sidebar order (generated: pages_registry.cpp)
const std::vector<PageInfo> &all_pages();
// pages left out of this build: (page id, the library target it needed)
const std::vector<std::pair<std::string, std::string>> &skipped_pages();

} // namespace fld
