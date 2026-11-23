#include "lsp.h"

#include <FL/Fl.H>

#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fld/util.h"

namespace lspc {

struct Client {
  std::vector<std::string> argv;
  std::string root;
  pid_t pid = -1;
  int in = -1, out = -1; // our ends: write to the server, read from it
  bool ready = false, dead = false;
  std::vector<std::string> queue; // sent once initialize has been answered
  lsp::Reader reader;
  int next_id = 1;
  std::map<int, std::function<void(const fld::Json &)>> pending;
  std::map<std::string, int> versions; // open documents
};

static std::vector<std::unique_ptr<Client>> g_clients;
static std::function<std::vector<std::string>(const std::string &)> g_resolver;
static std::function<void(const std::string &, const std::vector<lsp::Diagnostic> &)> g_diag;

void set_command_resolver(std::function<std::vector<std::string>(const std::string &)> f) { g_resolver = std::move(f); }
void set_diagnostics_handler(std::function<void(const std::string &, const std::vector<lsp::Diagnostic> &)> f) {
  g_diag = std::move(f);
}

static void write_all(Client *c, const std::string &msg) {
  if (c->dead || c->in < 0) return;
  std::string f = lsp::frame(msg);
  size_t off = 0;
  while (off < f.size()) {
    ssize_t n = write(c->in, f.data() + off, f.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;
    off += (size_t)n;
  }
}

static void send(Client *c, const std::string &msg) {
  if (c->dead) return;
  if (!c->ready) c->queue.push_back(msg);
  else write_all(c, msg);
}

static void request(Client *c, const std::function<std::string(int)> &make, std::function<void(const fld::Json &)> done) {
  if (c->dead) return;
  int id = c->next_id++;
  c->pending[id] = std::move(done);
  send(c, make(id));
}

static void stop(Client *c) {
  if (c->out >= 0) {
    Fl::remove_fd(c->out);
    close(c->out);
    c->out = -1;
  }
  if (c->in >= 0) {
    close(c->in);
    c->in = -1;
  }
  if (c->pid > 0) {
    int st;
    if (waitpid(c->pid, &st, WNOHANG) == 0) {
      kill(c->pid, SIGTERM);
      waitpid(c->pid, &st, 0);
    }
    c->pid = -1;
  }
  c->dead = true;
  c->pending.clear();
}

static void on_message(Client *c, const std::string &body) {
  lsp::Message m = lsp::parse(body);
  switch (m.kind) {
    case lsp::Message::Response: {
      auto it = c->pending.find(m.id);
      if (it == c->pending.end()) return;
      auto f = std::move(it->second);
      c->pending.erase(it);
      if (f) f(m.result);
      break;
    }
    case lsp::Message::ServerRequest:
      // progress, configuration, registration...: an empty answer keeps servers going
      write_all(c, lsp::reply_null(m.id));
      break;
    case lsp::Message::Diagnostics:
      if (g_diag) g_diag(lsp::uri_to_path(m.uri), m.diagnostics);
      break;
    default: break;
  }
}

static void readable(int fd, void *data) {
  Client *c = (Client *)data;
  char buf[65536];
  ssize_t n = read(fd, buf, sizeof buf);
  if (n <= 0) {
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    stop(c);
    return;
  }
  c->reader.feed(buf, (size_t)n);
  std::string body;
  while (!c->dead && c->reader.next(body)) on_message(c, body);
}

static bool start(Client *c) {
  int to[2], from[2];
  if (pipe(to) != 0) return false;
  if (pipe(from) != 0) {
    close(to[0]);
    close(to[1]);
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    dup2(to[0], 0);
    dup2(from[1], 1);
    int null = open("/dev/null", O_WRONLY);
    if (null >= 0) dup2(null, 2);
    for (int fd = 3; fd < 1024; fd++) close(fd);
    if (chdir(c->root.c_str()) != 0) {}
    std::vector<char *> av;
    for (auto &a : c->argv) av.push_back((char *)a.c_str());
    av.push_back(nullptr);
    execvp(av[0], av.data());
    _exit(127);
  }
  close(to[0]);
  close(from[1]);
  c->pid = pid;
  c->in = to[1];
  c->out = from[0];
  fcntl(c->in, F_SETFD, FD_CLOEXEC);
  fcntl(c->out, F_SETFD, FD_CLOEXEC);
  fcntl(c->out, F_SETFL, O_NONBLOCK);
  signal(SIGPIPE, SIG_IGN);
  Fl::add_fd(c->out, FL_READ, readable, c);
  int id = c->next_id++;
  c->pending[id] = [c](const fld::Json &) {
    c->ready = true;
    write_all(c, lsp::initialized());
    for (auto &m : c->queue) write_all(c, m);
    c->queue.clear();
  };
  write_all(c, lsp::initialize(id, (int)getpid(), lsp::path_to_uri(c->root)));
  return true;
}

std::string project_root(const std::string &path) {
  static const char *markers[] = {".git", "compile_commands.json", "Cargo.toml", "go.mod", "package.json",
                                  "pyproject.toml", "setup.py", "meson.build", ".hg", nullptr};
  std::string dir = fld::dirname(path);
  for (std::string d = dir; !d.empty() && d != "/"; d = fld::dirname(d)) {
    for (const char **m = markers; *m; m++) {
      struct stat st;
      if (stat((d + "/" + *m).c_str(), &st) == 0) return d;
    }
    if (d == fld::home_dir()) break;
  }
  return dir;
}

Client *attach(const std::string &path, const std::string &lang_id, const std::string &text) {
  if (path.empty() || lang_id.empty()) return nullptr;
  std::vector<std::string> argv = g_resolver ? g_resolver(lang_id) : lsp::default_server(lang_id);
  if (argv.empty() || fld::which(argv[0]).empty()) return nullptr;
  std::string root = project_root(path);
  Client *c = nullptr;
  for (auto &k : g_clients)
    if (!k->dead && k->argv == argv && k->root == root) c = k.get();
  if (!c) {
    auto n = std::make_unique<Client>();
    n->argv = argv;
    n->root = root;
    if (!start(n.get())) return nullptr;
    c = n.get();
    g_clients.push_back(std::move(n));
  }
  std::string uri = lsp::path_to_uri(path);
  if (!c->versions.count(uri)) {
    c->versions[uri] = 1;
    send(c, lsp::did_open(uri, lang_id, 1, text));
  }
  return c;
}

void change(Client *c, const std::string &path, const std::string &text) {
  if (!c) return;
  std::string uri = lsp::path_to_uri(path);
  auto it = c->versions.find(uri);
  if (it == c->versions.end()) return;
  send(c, lsp::did_change(uri, ++it->second, text));
}

void saved(Client *c, const std::string &path) {
  if (c) send(c, lsp::did_save(lsp::path_to_uri(path)));
}

void closed(Client *c, const std::string &path) {
  if (!c) return;
  std::string uri = lsp::path_to_uri(path);
  if (c->versions.erase(uri)) send(c, lsp::did_close(uri));
}

void complete(Client *c, const std::string &path, int line, int col16,
              std::function<void(const std::vector<lsp::CompletionItem> &)> done) {
  if (!c) return;
  std::string uri = lsp::path_to_uri(path);
  request(c, [&](int id) { return lsp::completion(id, uri, line, col16); },
          [done](const fld::Json &r) { done(lsp::parse_completion(r)); });
}

void definition(Client *c, const std::string &path, int line, int col16,
                std::function<void(const std::vector<lsp::Location> &)> done) {
  if (!c) return;
  std::string uri = lsp::path_to_uri(path);
  request(c, [&](int id) { return lsp::definition(id, uri, line, col16); },
          [done](const fld::Json &r) { done(lsp::parse_locations(r)); });
}

std::string name(const Client *c) { return c && !c->dead ? fld::basename(c->argv[0]) : ""; }

void shutdown_all() {
  for (auto &c : g_clients) {
    if (c->dead) continue;
    if (c->ready) {
      write_all(c.get(), lsp::shutdown(c->next_id++));
      write_all(c.get(), lsp::exit_notification());
      // give it a moment to exit by itself
      for (int i = 0; i < 20 && c->pid > 0; i++) {
        int st;
        if (waitpid(c->pid, &st, WNOHANG) == c->pid) {
          c->pid = -1;
          break;
        }
        usleep(10000);
      }
    }
    stop(c.get());
  }
}

} // namespace lspc
