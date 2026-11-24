#include "mdns.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/address.h>
#include <avahi-common/error.h>
#include <avahi-common/strlst.h>
#include <avahi-glib/glib-watch.h>
#include <glib.h>
#include <net/if.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>

#include "fld/glib_bridge.h"

namespace mdns {

struct Browser::Impl : std::enable_shared_from_this<Browser::Impl> {
  std::vector<std::string> types;
  std::function<void(const Service &)> added, removed;
  std::atomic<bool> alive{true};
  // GLib thread only
  AvahiGLibPoll *poll = nullptr;
  AvahiClient *client = nullptr;
  std::vector<AvahiServiceBrowser *> browsers;
  std::map<std::string, std::set<std::pair<int, int>>> seen; // key -> (interface, protocol)
  std::set<std::string> reported;
  // read from the FLTK thread too
  mutable std::mutex mu;
  std::map<std::string, Service> known;
  // keeps itself alive while Avahi holds the raw pointer
  std::shared_ptr<Impl> self;

  void report_added(const Service &s) {
    {
      std::lock_guard<std::mutex> l(mu);
      known[s.key()] = s;
    }
    auto me = shared_from_this();
    fld::ui_call([me, s] {
      if (me->alive && me->added) me->added(s);
    });
  }
  void report_removed(const std::string &key) {
    Service s;
    {
      std::lock_guard<std::mutex> l(mu);
      auto it = known.find(key);
      if (it == known.end()) return;
      s = it->second;
      known.erase(it);
    }
    auto me = shared_from_this();
    fld::ui_call([me, s] {
      if (me->alive && me->removed) me->removed(s);
    });
  }

  void free_browsers() {
    for (auto *b : browsers) avahi_service_browser_free(b);
    browsers.clear();
    seen.clear();
    reported.clear();
    std::vector<std::string> keys;
    {
      std::lock_guard<std::mutex> l(mu);
      for (auto &kv : known) keys.push_back(kv.first);
    }
    for (auto &k : keys) report_removed(k);
  }

  static void resolve_cb(AvahiServiceResolver *r, AvahiIfIndex iface, AvahiProtocol proto, AvahiResolverEvent ev,
                         const char *name, const char *type, const char *domain, const char *host,
                         const AvahiAddress *addr, uint16_t port, AvahiStringList *txt, AvahiLookupResultFlags,
                         void *ud) {
    Impl *me = (Impl *)ud;
    if (ev == AVAHI_RESOLVER_FOUND && me->alive) {
      Service s;
      s.name = name ? name : "";
      s.type = type ? type : "";
      s.domain = domain ? domain : "";
      s.host = host ? host : "";
      if (addr) {
        char a[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(a, sizeof a, addr);
        s.address = a;
        s.ipv6 = addr->proto == AVAHI_PROTO_INET6;
        // link-local IPv6 needs its interface
        if (s.ipv6 && s.address.compare(0, 5, "fe80:") == 0) {
          char ifn[IF_NAMESIZE] = {0};
          if (if_indextoname((unsigned)iface, ifn)) s.address += std::string("%") + ifn;
        }
      }
      s.port = port;
      s.interface = iface;
      std::vector<std::string> recs;
      for (AvahiStringList *l = txt; l; l = avahi_string_list_get_next(l))
        recs.emplace_back((const char *)avahi_string_list_get_text(l), avahi_string_list_get_size(l));
      s.txt = parse_txt(recs);
      // the same service on several interfaces / IPv4 and IPv6: once, IPv4 first
      std::string key = s.key();
      if (!me->reported.count(key)) {
        me->reported.insert(key);
        me->report_added(s);
      } else if (!s.ipv6) {
        std::lock_guard<std::mutex> l(me->mu);
        auto it = me->known.find(key);
        if (it != me->known.end() && it->second.ipv6) it->second = s;
      }
    }
    (void)proto;
    avahi_service_resolver_free(r);
  }

  static void browse_cb(AvahiServiceBrowser *, AvahiIfIndex iface, AvahiProtocol proto, AvahiBrowserEvent ev,
                        const char *name, const char *type, const char *domain, AvahiLookupResultFlags,
                        void *ud) {
    Impl *me = (Impl *)ud;
    if (!me->alive || !name || !type || !domain) return;
    Service probe;
    probe.name = name;
    probe.type = type;
    probe.domain = domain;
    std::string key = probe.key();
    if (ev == AVAHI_BROWSER_NEW) {
      me->seen[key].insert({iface, proto});
      avahi_service_resolver_new(me->client, iface, proto, name, type, domain, AVAHI_PROTO_UNSPEC,
                                 (AvahiLookupFlags)0, resolve_cb, me);
    } else if (ev == AVAHI_BROWSER_REMOVE) {
      auto it = me->seen.find(key);
      if (it == me->seen.end()) return;
      it->second.erase({iface, proto});
      if (it->second.empty()) {
        me->seen.erase(it);
        me->reported.erase(key);
        me->report_removed(key);
      }
    }
  }

  static void client_cb(AvahiClient *c, AvahiClientState state, void *ud) {
    Impl *me = (Impl *)ud;
    if (!me->client) me->client = c; // called from inside avahi_client_new
    if (state == AVAHI_CLIENT_S_RUNNING) {
      if (!me->browsers.empty()) return;
      for (auto &t : me->types) {
        AvahiServiceBrowser *b = avahi_service_browser_new(c, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, t.c_str(),
                                                           nullptr, (AvahiLookupFlags)0, browse_cb, me);
        if (b) me->browsers.push_back(b);
      }
    } else if (state == AVAHI_CLIENT_S_COLLISION || state == AVAHI_CLIENT_CONNECTING ||
               state == AVAHI_CLIENT_FAILURE) {
      // the daemon went away: AVAHI_CLIENT_NO_FAIL reconnects when it is back
      me->free_browsers();
    }
  }

  void start() {
    poll = avahi_glib_poll_new(g_main_context_get_thread_default(), G_PRIORITY_DEFAULT);
    if (!poll) return;
    int err = 0;
    AvahiClient *c = avahi_client_new(avahi_glib_poll_get(poll), AVAHI_CLIENT_NO_FAIL, client_cb, this, &err);
    if (c) client = c;
  }

  void stop() {
    free_browsers();
    if (client) avahi_client_free(client);
    client = nullptr;
    if (poll) avahi_glib_poll_free(poll);
    poll = nullptr;
  }
};

Browser::Browser(std::vector<std::string> types, std::function<void(const Service &)> added,
                 std::function<void(const Service &)> removed)
    : impl_(std::make_shared<Impl>()) {
  impl_->types = std::move(types);
  impl_->added = std::move(added);
  impl_->removed = std::move(removed);
  if (!fld::glib_running()) fld::glib_start();
  auto impl = impl_;
  fld::glib_call([impl] { impl->start(); });
}

Browser::~Browser() {
  impl_->alive = false;
  auto impl = impl_;
  // Avahi's objects are freed on the GLib thread; impl stays until then
  fld::glib_call([impl] { impl->stop(); });
}

std::vector<Service> Browser::services() const {
  std::lock_guard<std::mutex> l(impl_->mu);
  std::vector<Service> v;
  for (auto &kv : impl_->known) v.push_back(kv.second);
  return v;
}

bool daemon_running() {
  int err = 0;
  // a client without NO_FAIL fails at once when the daemon is not there
  AvahiGLibPoll *p = avahi_glib_poll_new(nullptr, G_PRIORITY_DEFAULT);
  if (!p) return false;
  AvahiClient *c = avahi_client_new(avahi_glib_poll_get(p), (AvahiClientFlags)0, nullptr, nullptr, &err);
  bool ok = c != nullptr;
  if (c) avahi_client_free(c);
  avahi_glib_poll_free(p);
  return ok;
}

} // namespace mdns
