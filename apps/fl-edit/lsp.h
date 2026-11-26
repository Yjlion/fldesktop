// fl-edit: language server processes. One server per command and project
// folder, shared by the documents in it; started on first use when the
// program is installed. Diagnostics arrive through the handler.
#pragma once
#include "lsp_util.h"
#include <functional>
#include <string>
#include <vector>

namespace lspc {

struct Client;

// the command for a language id ("" -> none); the default is lsp::default_server
void set_command_resolver(std::function<std::vector<std::string>(const std::string &lang_id)> f);
void set_diagnostics_handler(std::function<void(const std::string &path, const std::vector<lsp::Diagnostic> &)> f);

// opens a document on its server (starting it); nullptr when there is none
Client *attach(const std::string &path, const std::string &lang_id, const std::string &text);
void change(Client *c, const std::string &path, const std::string &text);
void saved(Client *c, const std::string &path);
void closed(Client *c, const std::string &path);
void complete(Client *c, const std::string &path, int line, int col16,
              std::function<void(const std::vector<lsp::CompletionItem> &)> done);
void definition(Client *c, const std::string &path, int line, int col16,
                std::function<void(const std::vector<lsp::Location> &)> done);
void format(Client *c, const std::string &path, int tab_size, bool insert_spaces,
            std::function<void(const std::vector<lsp::TextEdit> &)> done);
void rename(Client *c, const std::string &path, int line, int col16, const std::string &new_name,
            std::function<void(const std::vector<std::pair<std::string, std::vector<lsp::TextEdit>>> &, const std::string &error)> done);
std::string name(const Client *c); // e.g. "clangd"; "" when the server has exited
void shutdown_all();

// the project folder for a file: the nearest folder up with .git,
// compile_commands.json, Cargo.toml, go.mod, package.json, pyproject.toml...
std::string project_root(const std::string &path);

} // namespace lspc
