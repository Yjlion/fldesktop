// Runs a libFuzzer target over files without libFuzzer: each argument is a
// file or a directory of files (the seed corpus). Used by ctest when the
// fuzzers are not built with clang (FLD_FUZZ=OFF).
#include <dirent.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int run_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { fprintf(stderr, "cannot read %s\n", path.c_str()); return 1; }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string s = ss.str();
  LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t *>(s.data()), s.size());
  return 0;
}

int main(int argc, char **argv) {
  int n = 0, bad = 0;
  for (int i = 1; i < argc; i++) {
    struct stat st;
    if (stat(argv[i], &st) != 0) { fprintf(stderr, "missing %s\n", argv[i]); return 1; }
    if (S_ISDIR(st.st_mode)) {
      DIR *d = opendir(argv[i]);
      std::vector<std::string> files;
      while (dirent *e = d ? readdir(d) : nullptr)
        if (e->d_name[0] != '.') files.push_back(std::string(argv[i]) + "/" + e->d_name);
      if (d) closedir(d);
      for (auto &f : files) { bad += run_file(f); n++; }
    } else {
      bad += run_file(argv[i]);
      n++;
    }
  }
  // also the empty input and a few byte patterns
  const uint8_t junk[] = {0, 0xff, '[', '\n', '=', '{', '"', '\\', 0x80, 0xc3};
  LLVMFuzzerTestOneInput(nullptr, 0);
  for (size_t k = 1; k <= sizeof junk; k++) LLVMFuzzerTestOneInput(junk, k);
  printf("%d inputs\n", n);
  return bad ? 1 : 0;
}
