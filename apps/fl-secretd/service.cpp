// fl-secretd's D-Bus side: the Secret Service API (org.freedesktop.secrets)
// on the session bus. All objects live under /org/freedesktop/secrets and
// are served by one subtree:
//
//   /org/freedesktop/secrets                       Service
//   /org/freedesktop/secrets/collection/<c>        Collection
//   /org/freedesktop/secrets/collection/<c>/<id>   Item
//   /org/freedesktop/secrets/aliases/<alias>[/<id>] the same, by alias
//   /org/freedesktop/secrets/session/<n>           Session
//   /org/freedesktop/secrets/prompt/<n>            Prompt
#include "service.h"

#include <gio/gio.h>

#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "crypto.h"
#include "fld/ini.h"
#include "fld/util.h"
#include "store.h"

namespace secretd {

namespace {

const char *kRoot = "/org/freedesktop/secrets";
const char *kService = "org.freedesktop.Secret.Service";
const char *kCollection = "org.freedesktop.Secret.Collection";
const char *kItem = "org.freedesktop.Secret.Item";
const char *kSession = "org.freedesktop.Secret.Session";
const char *kPrompt = "org.freedesktop.Secret.Prompt";

const char *kXml = R"XML(<node>
 <interface name="org.freedesktop.Secret.Service">
  <method name="OpenSession"><arg name="algorithm" type="s" direction="in"/><arg name="input" type="v" direction="in"/>
   <arg name="output" type="v" direction="out"/><arg name="result" type="o" direction="out"/></method>
  <method name="CreateCollection"><arg name="properties" type="a{sv}" direction="in"/><arg name="alias" type="s" direction="in"/>
   <arg name="collection" type="o" direction="out"/><arg name="prompt" type="o" direction="out"/></method>
  <method name="SearchItems"><arg name="attributes" type="a{ss}" direction="in"/>
   <arg name="unlocked" type="ao" direction="out"/><arg name="locked" type="ao" direction="out"/></method>
  <method name="Unlock"><arg name="objects" type="ao" direction="in"/>
   <arg name="unlocked" type="ao" direction="out"/><arg name="prompt" type="o" direction="out"/></method>
  <method name="Lock"><arg name="objects" type="ao" direction="in"/>
   <arg name="locked" type="ao" direction="out"/><arg name="Prompt" type="o" direction="out"/></method>
  <method name="LockService"/>
  <method name="ChangeLock"><arg name="collection" type="o" direction="in"/><arg name="prompt" type="o" direction="out"/></method>
  <method name="GetSecrets"><arg name="items" type="ao" direction="in"/><arg name="session" type="o" direction="in"/>
   <arg name="secrets" type="a{o(oayays)}" direction="out"/></method>
  <method name="ReadAlias"><arg name="name" type="s" direction="in"/><arg name="collection" type="o" direction="out"/></method>
  <method name="SetAlias"><arg name="name" type="s" direction="in"/><arg name="collection" type="o" direction="in"/></method>
  <property name="Collections" type="ao" access="read"/>
  <signal name="CollectionCreated"><arg name="collection" type="o"/></signal>
  <signal name="CollectionDeleted"><arg name="collection" type="o"/></signal>
  <signal name="CollectionChanged"><arg name="collection" type="o"/></signal>
 </interface>
 <interface name="org.freedesktop.Secret.Collection">
  <method name="Delete"><arg name="prompt" type="o" direction="out"/></method>
  <method name="SearchItems"><arg name="attributes" type="a{ss}" direction="in"/><arg name="results" type="ao" direction="out"/></method>
  <method name="CreateItem"><arg name="properties" type="a{sv}" direction="in"/><arg name="secret" type="(oayays)" direction="in"/>
   <arg name="replace" type="b" direction="in"/><arg name="item" type="o" direction="out"/><arg name="prompt" type="o" direction="out"/></method>
  <property name="Items" type="ao" access="read"/>
  <property name="Label" type="s" access="readwrite"/>
  <property name="Locked" type="b" access="read"/>
  <property name="Created" type="t" access="read"/>
  <property name="Modified" type="t" access="read"/>
  <signal name="ItemCreated"><arg name="item" type="o"/></signal>
  <signal name="ItemDeleted"><arg name="item" type="o"/></signal>
  <signal name="ItemChanged"><arg name="item" type="o"/></signal>
 </interface>
 <interface name="org.freedesktop.Secret.Item">
  <method name="Delete"><arg name="Prompt" type="o" direction="out"/></method>
  <method name="GetSecret"><arg name="session" type="o" direction="in"/><arg name="secret" type="(oayays)" direction="out"/></method>
  <method name="SetSecret"><arg name="secret" type="(oayays)" direction="in"/></method>
  <property name="Locked" type="b" access="read"/>
  <property name="Attributes" type="a{ss}" access="readwrite"/>
  <property name="Label" type="s" access="readwrite"/>
  <property name="Type" type="s" access="readwrite"/>
  <property name="Created" type="t" access="read"/>
  <property name="Modified" type="t" access="read"/>
 </interface>
 <interface name="org.freedesktop.Secret.Session">
  <method name="Close"/>
 </interface>
 <interface name="org.freedesktop.Secret.Prompt">
  <method name="Prompt"><arg name="window-id" type="s" direction="in"/></method>
  <method name="Dismiss"/>
  <signal name="Completed"><arg name="dismissed" type="b"/><arg name="result" type="v"/></signal>
 </interface>
</node>)XML";

struct Session {
  std::string path, owner; // owner: the client's unique bus name
  bool dh = false;
  crypto::Bytes key;       // AES-128 key of a dh session
};

struct Prompt {
  std::string path, owner;
  // runs the prompt; finish(dismissed, result) when done (in the main loop)
  std::function<void(std::function<void(bool, GVariant *)>)> run;
  bool started = false;
};

struct State {
  Options opt;
  GDBusConnection *bus = nullptr;
  GDBusNodeInfo *info = nullptr;
  std::vector<std::unique_ptr<store::Collection>> collections;
  std::map<std::string, std::string> aliases; // alias -> collection name
  std::map<std::string, Session> sessions;    // path -> session
  std::map<std::string, std::shared_ptr<Prompt>> prompts;
  unsigned next_session = 1, next_prompt = 1;
  GMainLoop *loop = nullptr;
};
State *S = nullptr;

GDBusInterfaceInfo *iface(const char *name) { return g_dbus_node_info_lookup_interface(S->info, name); }

std::string coll_path(const store::Collection &c) { return std::string(kRoot) + "/collection/" + c.name; }
std::string item_path(const store::Collection &c, const store::Item &i) { return coll_path(c) + "/" + i.id; }

std::string aliases_path() { return S->opt.dir + "/aliases.ini"; }

void save_aliases() {
  fld::Ini ini;
  for (auto &kv : S->aliases) ini.set("Aliases", kv.first, kv.second);
  fld::mkdir_p(S->opt.dir, 0700);
  ini.save(aliases_path());
}

store::Collection *find_collection(const std::string &name) {
  for (auto &c : S->collections)
    if (c->name == name) return c.get();
  return nullptr;
}

// "collection/login" or "aliases/default", with or without an item
bool resolve(const std::string &node, store::Collection *&c, store::Item *&it, bool &is_item) {
  c = nullptr;
  it = nullptr;
  is_item = false;
  std::vector<std::string> parts = fld::split(node, '/');
  if (parts.size() < 2 || parts.size() > 3) return false;
  std::string name = parts[1];
  if (parts[0] == "aliases") {
    auto a = S->aliases.find(name);
    if (a == S->aliases.end()) return false;
    name = a->second;
  } else if (parts[0] != "collection") {
    return false;
  }
  c = find_collection(name);
  if (!c) return false;
  if (parts.size() == 3) {
    it = c->find(parts[2]);
    is_item = true;
    return it != nullptr;
  }
  return true;
}

// an object path from a client -> the relative node
std::string node_of(const std::string &path) {
  std::string root = kRoot;
  if (path.compare(0, root.size() + 1, root + "/") != 0) return "";
  return path.substr(root.size() + 1);
}

void emit(const std::string &path, const char *interface, const char *signal, GVariant *params) {
  g_dbus_connection_emit_signal(S->bus, nullptr, path.c_str(), interface, signal, params, nullptr);
}

void props_changed(const std::string &path, const char *interface, const char *prop, GVariant *value) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&b, "{sv}", prop, value);
  emit(path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
       g_variant_new("(sa{sv}as)", interface, &b, nullptr));
}

bool persist(store::Collection &c) {
  std::string err;
  if (!store::save(c, err)) {
    g_warning("fl-secretd: %s", err.c_str());
    return false;
  }
  return true;
}

GVariant *paths_variant(const std::vector<std::string> &paths) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("ao"));
  for (auto &p : paths) g_variant_builder_add(&b, "o", p.c_str());
  return g_variant_builder_end(&b);
}

std::vector<std::string> item_paths(const store::Collection &c) {
  std::vector<std::string> v;
  for (auto &i : c.items) v.push_back(item_path(c, i));
  return v;
}

store::Attributes attrs_from(GVariant *a) {
  store::Attributes m;
  GVariantIter it;
  const char *k, *v;
  g_variant_iter_init(&it, a);
  while (g_variant_iter_next(&it, "{&s&s}", &k, &v)) m[k] = v;
  return m;
}

GVariant *attrs_variant(const store::Attributes &a) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{ss}"));
  for (auto &kv : a) g_variant_builder_add(&b, "{ss}", kv.first.c_str(), kv.second.c_str());
  return g_variant_builder_end(&b);
}

GVariant *bytes_variant(const crypto::Bytes &b) {
  return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, b.data(), b.size(), 1);
}

crypto::Bytes bytes_of(GVariant *ay) {
  gsize n = 0;
  const char *d = (const char *)g_variant_get_fixed_array(ay, &n, 1);
  return crypto::Bytes(d ? d : "", n);
}

// the secret struct (oayays) for a session
GVariant *secret_variant(const Session &s, const store::Item &it) {
  crypto::Bytes params, value;
  if (s.dh) crypto::aes128_cbc_encrypt(s.key, it.secret, params, value);
  else value = it.secret;
  GVariant *r = g_variant_new("(o@ay@ays)", s.path.c_str(), bytes_variant(params), bytes_variant(value),
                              it.content_type.c_str());
  crypto::wipe(value);
  return r;
}

// a secret struct from a client -> the plain secret
bool secret_from(GVariant *sec, crypto::Bytes &out, std::string &content_type, const char *&error) {
  const char *session = nullptr, *ctype = nullptr;
  GVariant *params = nullptr, *value = nullptr;
  g_variant_get(sec, "(&o@ay@ay&s)", &session, &params, &value, &ctype);
  auto it = S->sessions.find(session);
  bool ok = false;
  error = "org.freedesktop.Secret.Error.NoSession";
  if (it != S->sessions.end()) {
    crypto::Bytes p = bytes_of(params), v = bytes_of(value);
    if (it->second.dh) {
      ok = crypto::aes128_cbc_decrypt(it->second.key, p, v, out);
      if (!ok) error = "org.freedesktop.DBus.Error.InvalidArgs";
    } else {
      out = v;
      ok = true;
    }
    crypto::wipe(v);
    content_type = ctype && *ctype ? ctype : "text/plain";
  }
  g_variant_unref(params);
  g_variant_unref(value);
  return ok;
}

// ---- prompts: the password dialog is a child process (fl-secretd --ask) -----

// asks in a dialog; done(password or none) when it closes
void ask(const std::vector<std::string> &args, std::function<void(bool ok, std::string pw)> done) {
  std::vector<const char *> argv = {S->opt.self.c_str(), "--ask"};
  for (auto &a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  GError *e = nullptr;
  GSubprocess *p = g_subprocess_newv(argv.data(), G_SUBPROCESS_FLAGS_STDOUT_PIPE, &e);
  if (!p) {
    g_warning("fl-secretd: cannot ask for the password: %s", e ? e->message : "?");
    g_clear_error(&e);
    done(false, "");
    return;
  }
  auto *cb = new std::function<void(bool, std::string)>(std::move(done));
  g_subprocess_communicate_utf8_async(p, nullptr, nullptr, [](GObject *src, GAsyncResult *res, gpointer d) {
    auto *cb = (std::function<void(bool, std::string)> *)d;
    GSubprocess *p = G_SUBPROCESS(src);
    char *out = nullptr;
    GError *e = nullptr;
    bool ok = g_subprocess_communicate_utf8_finish(p, res, &out, nullptr, &e) && g_subprocess_get_if_exited(p) &&
              g_subprocess_get_exit_status(p) == 0;
    std::string pw = out ? out : "";
    if (!pw.empty() && pw.back() == '\n') pw.pop_back();
    if (out) {
      memset(out, 0, strlen(out));
      g_free(out);
    }
    g_clear_error(&e);
    g_object_unref(p);
    (*cb)(ok, pw);
    crypto::wipe(pw);
    delete cb;
  }, cb);
}

std::string new_prompt(const std::string &owner, std::function<void(std::function<void(bool, GVariant *)>)> run) {
  auto p = std::make_shared<Prompt>();
  p->path = std::string(kRoot) + "/prompt/p" + std::to_string(S->next_prompt++);
  p->owner = owner;
  p->run = std::move(run);
  S->prompts[p->path] = p;
  return p->path;
}

void sync_objects();

void prompt_completed(const std::string &path, bool dismissed, GVariant *result) {
  auto it = S->prompts.find(path);
  if (it == S->prompts.end()) {
    if (result) g_variant_unref(g_variant_ref_sink(result));
    return;
  }
  std::string owner = it->second->owner;
  S->prompts.erase(it);
  if (!result) result = g_variant_new_string("");
  g_dbus_connection_emit_signal(S->bus, owner.empty() ? nullptr : owner.c_str(), path.c_str(), kPrompt, "Completed",
                                g_variant_new("(bv)", dismissed, result), nullptr);
  sync_objects(); // the prompt goes, a new collection or item may have come

}

// unlocks one collection: asks until the password is right or the user gives up
void unlock_collection(store::Collection *c, const std::string &error, std::function<void(bool)> done) {
  std::string name = c->name;
  std::vector<std::string> args = {"unlock", c->label.empty() ? c->name : c->label};
  if (!error.empty()) args.push_back(error);
  ask(args, [name, done](bool ok, std::string pw) {
    store::Collection *c = find_collection(name);
    if (!ok || !c) {
      done(false);
      return;
    }
    std::string err;
    if (!c->locked || store::unlock(*c, pw, err)) {
      props_changed(coll_path(*c), kCollection, "Locked", g_variant_new_boolean(FALSE));
      emit(kRoot, kService, "CollectionChanged", g_variant_new("(o)", coll_path(*c).c_str()));
      done(true);
      return;
    }
    unlock_collection(c, err == "wrong password" ? "The password was not right. Try again." : err, done);
  });
}

// ---- the methods ------------------------------------------------------------------

void reply_error(GDBusMethodInvocation *inv, const char *name, const std::string &msg) {
  g_dbus_method_invocation_return_dbus_error(inv, name, msg.c_str());
}

GVariant *collections_variant();

store::Collection *add_collection(const std::string &label, const std::string &password, std::string &err) {
  auto c = std::make_unique<store::Collection>();
  if (!store::create(*c, S->opt.dir, store::name_for(label, S->opt.dir), label, password, err)) return nullptr;
  store::Collection *raw = c.get();
  S->collections.push_back(std::move(c));
  emit(kRoot, kService, "CollectionCreated", g_variant_new("(o)", coll_path(*raw).c_str()));
  props_changed(kRoot, kService, "Collections", collections_variant());
  return raw;
}

GVariant *collections_variant() {
  std::vector<std::string> v;
  for (auto &c : S->collections) v.push_back(coll_path(*c));
  return paths_variant(v);
}

void service_method(GDBusMethodInvocation *inv, const char *sender, const char *method, GVariant *params) {
  if (!strcmp(method, "OpenSession")) {
    const char *alg = nullptr;
    GVariant *input = nullptr;
    g_variant_get(params, "(&sv)", &alg, &input);
    Session s;
    s.path = std::string(kRoot) + "/session/s" + std::to_string(S->next_session++);
    s.owner = sender ? sender : "";
    GVariant *output = nullptr;
    if (!strcmp(alg, "plain")) {
      output = g_variant_new_string("");
    } else if (!strcmp(alg, "dh-ietf1024-sha256-aes128-cbc-pkcs7") && g_variant_is_of_type(input, G_VARIANT_TYPE_BYTESTRING)) {
      crypto::DhKeys k;
      if (!crypto::dh_generate(k) || !crypto::dh_session_key(k, bytes_of(input), s.key)) {
        g_variant_unref(input);
        reply_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "invalid public key");
        return;
      }
      crypto::wipe(k.priv);
      s.dh = true;
      output = bytes_variant(k.pub);
    } else {
      g_variant_unref(input);
      reply_error(inv, "org.freedesktop.DBus.Error.NotSupported", std::string("algorithm not supported: ") + alg);
      return;
    }
    g_variant_unref(input);
    S->sessions[s.path] = s;
    g_dbus_method_invocation_return_value(inv, g_variant_new("(vo)", output, s.path.c_str()));
    return;
  }
  if (!strcmp(method, "SearchItems")) {
    GVariant *a = g_variant_get_child_value(params, 0);
    store::Attributes q = attrs_from(a);
    g_variant_unref(a);
    std::vector<std::string> unlocked, locked;
    for (auto &c : S->collections)
      for (auto &it : c->items)
        if (c->matches(it, q)) (c->locked ? locked : unlocked).push_back(item_path(*c, it));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@ao@ao)", paths_variant(unlocked), paths_variant(locked)));
    return;
  }
  if (!strcmp(method, "Unlock") || !strcmp(method, "Lock")) {
    bool unlock = !strcmp(method, "Unlock");
    GVariantIter *iter = nullptr;
    g_variant_get(params, "(ao)", &iter);
    const char *path;
    std::vector<std::string> done_paths;
    std::vector<std::string> to_unlock; // collection names
    std::vector<std::string> wanted;
    while (g_variant_iter_next(iter, "&o", &path)) {
      store::Collection *c;
      store::Item *it;
      bool is_item;
      if (!resolve(node_of(path), c, it, is_item)) continue;
      wanted.push_back(path);
      if (unlock) {
        if (c->locked && c->file.empty() == false) {
          if (std::find(to_unlock.begin(), to_unlock.end(), c->name) == to_unlock.end()) to_unlock.push_back(c->name);
        } else {
          done_paths.push_back(path);
        }
      } else {
        if (!c->locked && !c->file.empty()) {
          store::lock(*c);
          props_changed(coll_path(*c), kCollection, "Locked", g_variant_new_boolean(TRUE));
          emit(kRoot, kService, "CollectionChanged", g_variant_new("(o)", coll_path(*c).c_str()));
        }
        done_paths.push_back(path);
      }
    }
    g_variant_iter_free(iter);
    std::string prompt = "/";
    if (!to_unlock.empty()) {
      prompt = new_prompt(sender ? sender : "", [to_unlock, wanted](std::function<void(bool, GVariant *)> finish) {
        // one collection after another
        auto names = std::make_shared<std::vector<std::string>>(to_unlock);
        auto step = std::make_shared<std::function<void(size_t)>>();
        *step = [names, wanted, finish, step](size_t i) {
          if (i >= names->size()) {
            std::vector<std::string> ok;
            for (auto &w : wanted) {
              store::Collection *c;
              store::Item *it;
              bool is_item;
              if (resolve(node_of(w), c, it, is_item) && !c->locked) ok.push_back(w);
            }
            finish(false, paths_variant(ok));
            *step = nullptr; // breaks the cycle
            return;
          }
          store::Collection *c = find_collection((*names)[i]);
          if (!c) {
            (*step)(i + 1);
            return;
          }
          unlock_collection(c, "", [step, i, finish](bool ok) {
            if (!ok) {
              finish(true, paths_variant({}));
              *step = nullptr;
              return;
            }
            (*step)(i + 1);
          });
        };
        (*step)(0);
      });
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@aoo)", paths_variant(done_paths), prompt.c_str()));
    return;
  }
  if (!strcmp(method, "LockService")) {
    for (auto &c : S->collections)
      if (!c->locked && !c->file.empty()) store::lock(*c);
    g_dbus_method_invocation_return_value(inv, nullptr);
    return;
  }
  if (!strcmp(method, "GetSecrets")) {
    GVariantIter *iter = nullptr;
    const char *session = nullptr;
    g_variant_get(params, "(ao&o)", &iter, &session);
    auto s = S->sessions.find(session);
    if (s == S->sessions.end()) {
      g_variant_iter_free(iter);
      reply_error(inv, "org.freedesktop.Secret.Error.NoSession", "no such session");
      return;
    }
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{o(oayays)}"));
    const char *path;
    while (g_variant_iter_next(iter, "&o", &path)) {
      store::Collection *c;
      store::Item *it;
      bool is_item;
      if (!resolve(node_of(path), c, it, is_item) || !is_item || c->locked) continue;
      g_variant_builder_add(&b, "{o@(oayays)}", path, secret_variant(s->second, *it));
    }
    g_variant_iter_free(iter);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(a{o(oayays)})", &b));
    return;
  }
  if (!strcmp(method, "ReadAlias")) {
    const char *name = nullptr;
    g_variant_get(params, "(&s)", &name);
    auto a = S->aliases.find(name);
    store::Collection *c = a == S->aliases.end() ? nullptr : find_collection(a->second);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", c ? coll_path(*c).c_str() : "/"));
    return;
  }
  if (!strcmp(method, "SetAlias")) {
    const char *name = nullptr, *path = nullptr;
    g_variant_get(params, "(&s&o)", &name, &path);
    if (!strcmp(path, "/")) S->aliases.erase(name);
    else {
      store::Collection *c;
      store::Item *it;
      bool is_item;
      if (!resolve(node_of(path), c, it, is_item) || is_item) {
        reply_error(inv, "org.freedesktop.Secret.Error.NoSuchObject", "no such collection");
        return;
      }
      S->aliases[name] = c->name;
    }
    save_aliases();
    g_dbus_method_invocation_return_value(inv, nullptr);
    return;
  }
  if (!strcmp(method, "CreateCollection")) {
    GVariant *props = g_variant_get_child_value(params, 0);
    const char *alias = nullptr;
    g_variant_get_child(params, 1, "&s", &alias);
    std::string label = "Passwords", al = alias ? alias : "";
    GVariant *l = g_variant_lookup_value(props, "org.freedesktop.Secret.Collection.Label", G_VARIANT_TYPE_STRING);
    if (l) {
      label = g_variant_get_string(l, nullptr);
      g_variant_unref(l);
    }
    g_variant_unref(props);
    // an alias that exists already: that collection
    if (!al.empty() && S->aliases.count(al) && find_collection(S->aliases[al])) {
      g_dbus_method_invocation_return_value(
          inv, g_variant_new("(oo)", coll_path(*find_collection(S->aliases[al])).c_str(), "/"));
      return;
    }
    std::string prompt = new_prompt(sender ? sender : "", [label, al](std::function<void(bool, GVariant *)> finish) {
      ask({"new", label}, [label, al, finish](bool ok, std::string pw) {
        if (!ok) {
          finish(true, g_variant_new_object_path("/"));
          return;
        }
        std::string err;
        store::Collection *c = add_collection(label, pw, err);
        if (!c) {
          g_warning("fl-secretd: %s", err.c_str());
          finish(true, g_variant_new_object_path("/"));
          return;
        }
        if (!al.empty()) {
          S->aliases[al] = c->name;
          save_aliases();
        }
        finish(false, g_variant_new_object_path(coll_path(*c).c_str()));
      });
    });
    g_dbus_method_invocation_return_value(inv, g_variant_new("(oo)", "/", prompt.c_str()));
    return;
  }
  if (!strcmp(method, "ChangeLock")) {
    const char *path = nullptr;
    g_variant_get(params, "(&o)", &path);
    store::Collection *c;
    store::Item *it;
    bool is_item;
    if (!resolve(node_of(path), c, it, is_item) || is_item || c->file.empty()) {
      reply_error(inv, "org.freedesktop.Secret.Error.NoSuchObject", "no such collection");
      return;
    }
    std::string name = c->name;
    std::string prompt = new_prompt(sender ? sender : "", [name](std::function<void(bool, GVariant *)> finish) {
      store::Collection *c = find_collection(name);
      if (!c) return finish(true, nullptr);
      ask({"change", c->label}, [name, finish](bool ok, std::string both) {
        store::Collection *c = find_collection(name);
        size_t nl = both.find('\n');
        std::string err;
        if (!ok || !c || nl == std::string::npos ||
            !store::change_password(*c, both.substr(0, nl), both.substr(nl + 1), err)) {
          if (!err.empty()) g_warning("fl-secretd: %s", err.c_str());
          finish(true, nullptr);
          return;
        }
        finish(false, nullptr);
      });
    });
    g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", prompt.c_str()));
    return;
  }
  reply_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

void delete_item(store::Collection *c, const std::string &id) {
  store::Item *it = c->find(id);
  if (!it) return;
  std::string path = item_path(*c, *it);
  crypto::wipe(it->secret);
  c->items.erase(c->items.begin() + (it - c->items.data()));
  c->modified = store::now();
  persist(*c);
  emit(coll_path(*c), kCollection, "ItemDeleted", g_variant_new("(o)", path.c_str()));
}

// a new item, or the one with the same attributes when replace is set
std::string create_item(store::Collection *c, const std::string &label, const store::Attributes &attrs,
                        const crypto::Bytes &secret, const std::string &ctype, bool replace) {
  store::Item *it = nullptr;
  if (replace)
    for (auto &x : c->items)
      if (x.attributes == attrs) it = &x;
  bool created = !it;
  if (created) {
    c->items.push_back(store::Item());
    it = &c->items.back();
    it->id = c->new_id();
    it->created = store::now();
  }
  it->label = label;
  it->content_type = ctype;
  c->set_attributes(*it, attrs);
  crypto::wipe(it->secret);
  it->secret = secret;
  it->modified = c->modified = store::now();
  std::string path = item_path(*c, *it);
  persist(*c);
  emit(coll_path(*c), kCollection, created ? "ItemCreated" : "ItemChanged", g_variant_new("(o)", path.c_str()));
  return path;
}

void collection_method(GDBusMethodInvocation *inv, const char *sender, store::Collection *c, const char *method,
                       GVariant *params) {
  if (!strcmp(method, "SearchItems")) {
    GVariant *a = g_variant_get_child_value(params, 0);
    store::Attributes q = attrs_from(a);
    g_variant_unref(a);
    std::vector<std::string> v;
    for (auto &it : c->items)
      if (c->matches(it, q)) v.push_back(item_path(*c, it));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@ao)", paths_variant(v)));
    return;
  }
  if (!strcmp(method, "Delete")) {
    std::string path = coll_path(*c);
    if (!c->file.empty()) unlink(c->file.c_str());
    for (auto it = S->aliases.begin(); it != S->aliases.end();)
      it = it->second == c->name ? S->aliases.erase(it) : std::next(it);
    save_aliases();
    store::lock(*c);
    S->collections.erase(std::remove_if(S->collections.begin(), S->collections.end(),
                                        [c](const std::unique_ptr<store::Collection> &x) { return x.get() == c; }),
                         S->collections.end());
    emit(kRoot, kService, "CollectionDeleted", g_variant_new("(o)", path.c_str()));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", "/"));
    return;
  }
  if (!strcmp(method, "CreateItem")) {
    GVariant *props = g_variant_get_child_value(params, 0);
    GVariant *sec = g_variant_get_child_value(params, 1);
    gboolean replace = FALSE;
    g_variant_get_child(params, 2, "b", &replace);
    std::string label;
    store::Attributes attrs;
    if (GVariant *l = g_variant_lookup_value(props, "org.freedesktop.Secret.Item.Label", G_VARIANT_TYPE_STRING)) {
      label = g_variant_get_string(l, nullptr);
      g_variant_unref(l);
    }
    if (GVariant *a = g_variant_lookup_value(props, "org.freedesktop.Secret.Item.Attributes", G_VARIANT_TYPE("a{ss}"))) {
      attrs = attrs_from(a);
      g_variant_unref(a);
    }
    g_variant_unref(props);
    crypto::Bytes secret;
    std::string ctype;
    const char *error = nullptr;
    bool ok = secret_from(sec, secret, ctype, error);
    g_variant_unref(sec);
    if (!ok) {
      reply_error(inv, error, "cannot read the secret");
      return;
    }
    if (!c->locked) {
      std::string path = create_item(c, label, attrs, secret, ctype, replace);
      crypto::wipe(secret);
      g_dbus_method_invocation_return_value(inv, g_variant_new("(oo)", path.c_str(), "/"));
      return;
    }
    // locked: unlock it first, in a prompt
    std::string name = c->name;
    auto keep = std::make_shared<crypto::Bytes>(secret);
    crypto::wipe(secret);
    std::string prompt = new_prompt(sender ? sender : "", [name, label, attrs, keep, ctype, replace](
                                                                 std::function<void(bool, GVariant *)> finish) {
      store::Collection *c = find_collection(name);
      if (!c) return finish(true, g_variant_new_object_path("/"));
      unlock_collection(c, "", [name, label, attrs, keep, ctype, replace, finish](bool ok) {
        store::Collection *c = find_collection(name);
        if (!ok || !c) {
          crypto::wipe(*keep);
          finish(true, g_variant_new_object_path("/"));
          return;
        }
        std::string path = create_item(c, label, attrs, *keep, ctype, replace);
        crypto::wipe(*keep);
        finish(false, g_variant_new_object_path(path.c_str()));
      });
    });
    g_dbus_method_invocation_return_value(inv, g_variant_new("(oo)", "/", prompt.c_str()));
    return;
  }
  reply_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

void item_method(GDBusMethodInvocation *inv, store::Collection *c, store::Item *it, const char *method,
                 GVariant *params) {
  if (!strcmp(method, "Delete")) {
    if (c->locked) return reply_error(inv, "org.freedesktop.Secret.Error.IsLocked", "the keyring is locked");
    delete_item(c, it->id);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", "/"));
    return;
  }
  if (!strcmp(method, "GetSecret")) {
    const char *session = nullptr;
    g_variant_get(params, "(&o)", &session);
    auto s = S->sessions.find(session);
    if (s == S->sessions.end()) return reply_error(inv, "org.freedesktop.Secret.Error.NoSession", "no such session");
    if (c->locked) return reply_error(inv, "org.freedesktop.Secret.Error.IsLocked", "the keyring is locked");
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@(oayays))", secret_variant(s->second, *it)));
    return;
  }
  if (!strcmp(method, "SetSecret")) {
    if (c->locked) return reply_error(inv, "org.freedesktop.Secret.Error.IsLocked", "the keyring is locked");
    GVariant *sec = g_variant_get_child_value(params, 0);
    crypto::Bytes secret;
    std::string ctype;
    const char *error = nullptr;
    bool ok = secret_from(sec, secret, ctype, error);
    g_variant_unref(sec);
    if (!ok) return reply_error(inv, error, "cannot read the secret");
    crypto::wipe(it->secret);
    it->secret = secret;
    it->content_type = ctype;
    crypto::wipe(secret);
    it->modified = c->modified = store::now();
    persist(*c);
    emit(coll_path(*c), kCollection, "ItemChanged", g_variant_new("(o)", item_path(*c, *it).c_str()));
    g_dbus_method_invocation_return_value(inv, nullptr);
    return;
  }
  reply_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

// ---- the subtree ------------------------------------------------------------------

void method_call_inner(const char *sender, const char *path, const char *interface, const char *method,
                       GVariant *params, GDBusMethodInvocation *inv) {
  std::string node = node_of(path);
  if (!strcmp(interface, kService)) return service_method(inv, sender, method, params);
  if (!strcmp(interface, kSession)) {
    S->sessions.erase(path);
    g_dbus_method_invocation_return_value(inv, nullptr);
    return;
  }
  if (!strcmp(interface, kPrompt)) {
    auto it = S->prompts.find(path);
    if (it == S->prompts.end()) return reply_error(inv, "org.freedesktop.Secret.Error.NoSuchObject", "no such prompt");
    std::shared_ptr<Prompt> p = it->second;
    if (!strcmp(method, "Dismiss")) {
      g_dbus_method_invocation_return_value(inv, nullptr);
      prompt_completed(p->path, true, nullptr);
      return;
    }
    g_dbus_method_invocation_return_value(inv, nullptr);
    if (p->started) return;
    p->started = true;
    std::string pp = p->path;
    p->run([pp](bool dismissed, GVariant *result) { prompt_completed(pp, dismissed, result); });
    return;
  }
  store::Collection *c;
  store::Item *it;
  bool is_item;
  if (!resolve(node, c, it, is_item)) return reply_error(inv, "org.freedesktop.Secret.Error.NoSuchObject", "no such object");
  if (!strcmp(interface, kCollection) && !is_item) return collection_method(inv, sender, c, method, params);
  if (!strcmp(interface, kItem) && is_item) return item_method(inv, c, it, method, params);
  reply_error(inv, "org.freedesktop.DBus.Error.UnknownInterface", interface);
}

void method_call(GDBusConnection *, const char *sender, const char *path, const char *interface, const char *method,
                 GVariant *params, GDBusMethodInvocation *inv, gpointer) {
  method_call_inner(sender, path, interface, method, params, inv);
  sync_objects();
}

GVariant *get_property(GDBusConnection *, const char *, const char *path, const char *interface, const char *prop,
                       GError **error, gpointer) {
  if (!strcmp(interface, kService)) {
    if (!strcmp(prop, "Collections")) return collections_variant();
  }
  store::Collection *c;
  store::Item *it;
  bool is_item;
  if (resolve(node_of(path), c, it, is_item)) {
    if (!strcmp(interface, kCollection) && !is_item) {
      if (!strcmp(prop, "Items")) return paths_variant(item_paths(*c));
      if (!strcmp(prop, "Label")) return g_variant_new_string(c->label.c_str());
      if (!strcmp(prop, "Locked")) return g_variant_new_boolean(c->locked);
      if (!strcmp(prop, "Created")) return g_variant_new_uint64(c->created);
      if (!strcmp(prop, "Modified")) return g_variant_new_uint64(c->modified);
    }
    if (!strcmp(interface, kItem) && is_item) {
      if (!strcmp(prop, "Locked")) return g_variant_new_boolean(c->locked);
      if (!strcmp(prop, "Attributes")) return attrs_variant(it->attributes);
      if (!strcmp(prop, "Label")) return g_variant_new_string(it->label.c_str());
      if (!strcmp(prop, "Type")) return g_variant_new_string(it->content_type.c_str());
      if (!strcmp(prop, "Created")) return g_variant_new_uint64(it->created);
      if (!strcmp(prop, "Modified")) return g_variant_new_uint64(it->modified);
    }
  }
  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s", prop);
  return nullptr;
}

gboolean set_property(GDBusConnection *, const char *, const char *path, const char *interface, const char *prop,
                      GVariant *value, GError **error, gpointer) {
  store::Collection *c;
  store::Item *it;
  bool is_item;
  if (!resolve(node_of(path), c, it, is_item)) {
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT, "no such object");
    return FALSE;
  }
  if (c->locked) {
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "the keyring is locked");
    return FALSE;
  }
  if (!strcmp(interface, kCollection) && !is_item && !strcmp(prop, "Label")) {
    c->label = g_variant_get_string(value, nullptr);
  } else if (!strcmp(interface, kItem) && is_item && !strcmp(prop, "Label")) {
    it->label = g_variant_get_string(value, nullptr);
  } else if (!strcmp(interface, kItem) && is_item && !strcmp(prop, "Type")) {
    it->content_type = g_variant_get_string(value, nullptr);
  } else if (!strcmp(interface, kItem) && is_item && !strcmp(prop, "Attributes")) {
    c->set_attributes(*it, attrs_from(value));
  } else {
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY, "%s cannot be changed", prop);
    return FALSE;
  }
  if (it) it->modified = store::now();
  c->modified = store::now();
  persist(*c);
  if (is_item) emit(coll_path(*c), kCollection, "ItemChanged", g_variant_new("(o)", item_path(*c, *it).c_str()));
  return TRUE;
}

const GDBusInterfaceVTable kVtable = {method_call, get_property, set_property, {nullptr}};

// Every object is registered on its own (GDBus subtrees reach only direct
// children): the registrations follow the collections, items, aliases,
// sessions and prompts after each change.
std::map<std::string, guint> g_registered; // path -> registration id

void sync_objects() {
  std::map<std::string, const char *> want; // path -> interface
  want[kRoot] = kService;
  auto add_coll = [&](const std::string &base, const store::Collection &c) {
    want[base] = kCollection;
    for (auto &it : c.items) want[base + "/" + it.id] = kItem;
  };
  for (auto &c : S->collections) add_coll(coll_path(*c), *c);
  for (auto &a : S->aliases)
    if (store::Collection *c = find_collection(a.second)) add_coll(std::string(kRoot) + "/aliases/" + a.first, *c);
  for (auto &s : S->sessions) want[s.first] = kSession;
  for (auto &p : S->prompts) want[p.first] = kPrompt;
  for (auto it = g_registered.begin(); it != g_registered.end();) {
    if (!want.count(it->first)) {
      g_dbus_connection_unregister_object(S->bus, it->second);
      it = g_registered.erase(it);
    } else {
      ++it;
    }
  }
  for (auto &w : want) {
    if (g_registered.count(w.first)) continue;
    GError *e = nullptr;
    guint id = g_dbus_connection_register_object(S->bus, w.first.c_str(), iface(w.second), &kVtable, nullptr, nullptr, &e);
    if (id) g_registered[w.first] = id;
    else g_warning("fl-secretd: cannot register %s: %s", w.first.c_str(), e ? e->message : "?");
    g_clear_error(&e);
  }
}

// a client went away: its sessions and prompts go too
void name_owner_changed(GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *params,
                        gpointer) {
  const char *name, *old_owner, *new_owner;
  g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
  if (*new_owner || name[0] != ':') return;
  for (auto it = S->sessions.begin(); it != S->sessions.end();)
    it = it->second.owner == name ? S->sessions.erase(it) : std::next(it);
  sync_objects();
}

void load_all() {
  fld::mkdir_p(S->opt.dir, 0700);
  for (auto &f : fld::list_dir(S->opt.dir, false)) {
    if (!fld::ends_with(f, ".keyring")) continue;
    auto c = std::make_unique<store::Collection>();
    std::string err;
    if (store::load(*c, S->opt.dir + "/" + f, err)) S->collections.push_back(std::move(c));
    else g_warning("fl-secretd: %s: %s", f.c_str(), err.c_str());
  }
  fld::Ini ini;
  ini.load(aliases_path());
  for (auto &k : ini.keys("Aliases")) S->aliases[k] = ini.get("Aliases", k);
  // the login keyring is the default one unless something else was chosen
  if (!S->aliases.count("default") && find_collection("login")) S->aliases["default"] = "login";
  // "session": in memory, gone at logout
  auto s = std::make_unique<store::Collection>();
  s->name = "session";
  s->label = "Session";
  s->locked = false;
  s->hash_salt = crypto::random_bytes(16);
  s->created = s->modified = store::now();
  S->aliases["session"] = "session";
  S->collections.push_back(std::move(s));
}

} // namespace

bool unlock_with_key(const std::string &collection, const crypto::Bytes &key) {
  if (!S) return false;
  store::Collection *c = find_collection(collection);
  std::string err;
  if (!c || !c->locked) return c != nullptr;
  if (!store::unlock_with_key(*c, key, err)) {
    g_warning("fl-secretd: cannot unlock %s: %s", collection.c_str(), err.c_str());
    return false;
  }
  if (!S->aliases.count("default")) {
    S->aliases["default"] = c->name;
    save_aliases();
  }
  sync_objects();
  return true;
}

void reload_collection(const std::string &name) {
  if (!S || find_collection(name)) return;
  auto c = std::make_unique<store::Collection>();
  std::string err;
  if (store::load(*c, S->opt.dir + "/" + name + ".keyring", err)) {
    S->collections.push_back(std::move(c));
    if (name == "login" && !S->aliases.count("default")) S->aliases["default"] = "login";
    sync_objects();
  }
}

int run(const Options &opt, std::function<void()> on_ready) {
  State st;
  st.opt = opt;
  S = &st;
  GError *e = nullptr;
  st.info = g_dbus_node_info_new_for_xml(kXml, &e);
  if (!st.info) {
    g_printerr("fl-secretd: %s\n", e->message);
    return 1;
  }
  st.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &e);
  if (!st.bus) {
    g_printerr("fl-secretd: no session bus: %s\n", e->message);
    return 1;
  }
  load_all();
  sync_objects();
  g_dbus_connection_signal_subscribe(st.bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                                     "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE, name_owner_changed,
                                     nullptr, nullptr);
  st.loop = g_main_loop_new(nullptr, FALSE);
  // the name last: clients may call as soon as they see it
  struct Ctx {
    std::function<void()> ready;
    GMainLoop *loop;
  } ctx{on_ready, st.loop};
  guint own = g_bus_own_name_on_connection(
      st.bus, "org.freedesktop.secrets", G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
      [](GDBusConnection *, const char *, gpointer d) {
        Ctx *c = (Ctx *)d;
        if (c->ready) c->ready();
      },
      [](GDBusConnection *, const char *, gpointer d) {
        // another Secret Service (GNOME Keyring, KeePassXC) has the name
        g_printerr("fl-secretd: org.freedesktop.secrets is taken by another program\n");
        g_main_loop_quit(((Ctx *)d)->loop);
      },
      &ctx, nullptr);
  g_main_loop_run(st.loop);
  g_bus_unown_name(own);
  for (auto &r : g_registered) g_dbus_connection_unregister_object(st.bus, r.second);
  g_registered.clear();
  for (auto &c : st.collections) store::lock(*c);
  g_main_loop_unref(st.loop);
  g_dbus_node_info_unref(st.info);
  g_object_unref(st.bus);
  S = nullptr;
  return 1;
}

} // namespace secretd
