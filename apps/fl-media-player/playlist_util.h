// fl-media-player: playlist files and play order (no FLTK or GStreamer)
#pragma once
#include <string>
#include <vector>

namespace media {

struct Entry {
  std::string location; // a path or a URL
  std::string title;    // from #EXTINF / TitleN, may be ""
  double length = -1;   // seconds, -1 unknown
};

// .m3u, .m3u8 and .pls (by the name)
bool is_playlist(const std::string &path);
// A playlist file's entries; relative paths are taken from `dir` (the file's
// folder). `pls` picks the format (else M3U, with or without #EXTM3U).
std::vector<Entry> parse_playlist(const std::string &text, const std::string &dir, bool pls);
// The text of a playlist file (extended M3U, or PLS version 2).
std::string write_m3u(const std::vector<Entry> &entries);
std::string write_pls(const std::vector<Entry> &entries);

// A shuffled play order of n items that starts with `first` (-1: any);
// the same seed gives the same order.
std::vector<int> shuffle_order(int n, int first, unsigned seed);

// Repeat: off (stop at the end), all (start again), one (the same item).
enum class Repeat { Off, All, One };
Repeat next_repeat(Repeat r);
// The item after `cur` in `order` (indices into the playlist), or -1 at the
// end; `dir` -1 goes back.
int step(const std::vector<int> &order, int cur, int dir, Repeat repeat);

} // namespace media
