// fl-launcher: results other than applications for what was typed (no FLTK;
// unit tested):
//   2*(3+4)          = 14               (fl-calc's evaluator; Enter copies it)
//   10 km in mi      = 6.21371 mi       (also "to"; length, mass, volume,
//                                         temperature, data, time, speed)
//   >htop            run htop in a terminal
//   notes.txt        files with that name (plocate/locate, when installed)
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace launcher {

struct Extra {
  enum Kind { Calc, Convert, Command, File } kind;
  std::string title, subtitle, icon;
  std::string copy;                // text Enter copies (Calc, Convert)
  std::vector<std::string> argv;   // what Enter runs (Command, File)
};

// the calculator: an expression with a number and an operator or function
bool calculate(const std::string &query, Extra &out);
// "<number> <unit> in|to <unit>"
bool convert(const std::string &query, Extra &out);
// ">command": run it in the terminal; terminal is the terminal's argv prefix
bool command(const std::string &query, const std::vector<std::string> &terminal, Extra &out);
// file results from a locate tool's output (one path per line); at most max
std::vector<Extra> files_from_locate(const std::string &output, size_t max = 8);
// the argv that asks locate for names containing the query ("" when no
// locate is installed or the query is too short)
std::vector<std::string> locate_command(const std::string &query, const std::string &locate_path);
// all quick results (calculator, conversion, command), best first
std::vector<Extra> quick(const std::string &query, const std::vector<std::string> &terminal);

} // namespace launcher
