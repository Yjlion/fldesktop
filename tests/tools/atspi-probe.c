/* Finds an application on the accessibility bus through the AT-SPI registry
 * and checks the bridge: the tree, actions, text, values, states and focus
 * events (tests/atspi-smoke.sh). usage: atspi-probe APP-NAME */
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

static GDBusConnection *bus;
static int failures;

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } while (0)

typedef struct { char bus[128], path[128], name[128]; unsigned role; } Obj;
static Obj objs[512];
static int nobjs;

static GVariant *call(const char *dest, const char *path, const char *iface, const char *method, GVariant *args,
                      const char *type) {
  GError *e = NULL;
  GVariant *r = g_dbus_connection_call_sync(bus, dest, path, iface, method, args, type ? G_VARIANT_TYPE(type) : NULL,
                                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &e);
  if (!r) {
    fprintf(stderr, "%s.%s on %s %s: %s\n", iface, method, dest, path, e ? e->message : "?");
    if (e) g_error_free(e);
  }
  return r;
}

static GVariant *prop(const char *dest, const char *path, const char *iface, const char *name) {
  GVariant *r = call(dest, path, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", iface, name), "(v)");
  if (!r) return NULL;
  GVariant *v;
  g_variant_get(r, "(v)", &v);
  g_variant_unref(r);
  return v;
}

static void walk(const char *dest, const char *path, int depth) {
  if (nobjs >= 512 || depth > 20) return;
  Obj *o = &objs[nobjs++];
  snprintf(o->bus, sizeof o->bus, "%s", dest);
  snprintf(o->path, sizeof o->path, "%s", path);
  GVariant *n = prop(dest, path, "org.a11y.atspi.Accessible", "Name");
  snprintf(o->name, sizeof o->name, "%s", n ? g_variant_get_string(n, NULL) : "");
  if (n) g_variant_unref(n);
  GVariant *r = call(dest, path, "org.a11y.atspi.Accessible", "GetRole", NULL, "(u)");
  if (r) {
    g_variant_get(r, "(u)", &o->role);
    g_variant_unref(r);
  }
  printf("%*s%u %s\n", depth * 2, "", o->role, o->name);
  r = call(dest, path, "org.a11y.atspi.Accessible", "GetChildren", NULL, "(a(so))");
  if (!r) return;
  GVariantIter *it;
  const char *cb, *cp;
  g_variant_get(r, "(a(so))", &it);
  char kids[64][128];
  int nk = 0;
  while (g_variant_iter_next(it, "(&s&o)", &cb, &cp) && nk < 64) snprintf(kids[nk++], 128, "%s", cp);
  g_variant_iter_free(it);
  g_variant_unref(r);
  for (int i = 0; i < nk; i++) walk(dest, kids[i], depth + 1);
}

static Obj *find(const char *name, unsigned role) {
  for (int i = 0; i < nobjs; i++)
    if (!strcmp(objs[i].name, name) && (!role || objs[i].role == role)) return &objs[i];
  FAIL("no '%s' (role %u) in the tree", name, role);
  return NULL;
}

static unsigned long long states(Obj *o) {
  GVariant *r = call(o->bus, o->path, "org.a11y.atspi.Accessible", "GetState", NULL, "(au)");
  if (!r) return 0;
  GVariantIter *it;
  guint32 w, k = 0;
  unsigned long long s = 0;
  g_variant_get(r, "(au)", &it);
  while (g_variant_iter_next(it, "u", &w)) s |= (unsigned long long)w << (32 * k++);
  g_variant_iter_free(it);
  g_variant_unref(r);
  return s;
}

static char *text(Obj *o) {
  GVariant *r = call(o->bus, o->path, "org.a11y.atspi.Text", "GetText", g_variant_new("(ii)", 0, -1), "(s)");
  char *s = NULL;
  if (r) {
    g_variant_get(r, "(s)", &s);
    g_variant_unref(r);
  }
  return s ? s : g_strdup("");
}

static void wait(int ms) {
  gint64 end = g_get_monotonic_time() + ms * 1000;
  while (g_get_monotonic_time() < end) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(20000);
  }
}

static char focused_path[128];
static void on_state(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *sig,
                     GVariant *p, gpointer d) {
  const char *detail;
  int v1, v2;
  GVariant *any, *props;
  g_variant_get(p, "(&sii@v@a{sv})", &detail, &v1, &v2, &any, &props);
  if (!strcmp(detail, "focused") && v1 == 1) snprintf(focused_path, sizeof focused_path, "%s", path);
  g_variant_unref(any);
  g_variant_unref(props);
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  const char *addr = g_getenv("AT_SPI_BUS_ADDRESS");
  GError *e = NULL;
  bus = addr ? g_dbus_connection_new_for_address_sync(addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                      NULL, NULL, &e)
             : g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
  if (!bus) {
    fprintf(stderr, "no bus: %s\n", e ? e->message : "?");
    return 1;
  }
  g_dbus_connection_signal_subscribe(bus, NULL, "org.a11y.atspi.Event.Object", "StateChanged", NULL, NULL,
                                     G_DBUS_SIGNAL_FLAGS_NONE, on_state, NULL, NULL);
  /* the application, as the registry lists it */
  char app_bus[128] = "";
  for (int tries = 0; tries < 50 && !*app_bus; tries++) {
    GVariant *r = call("org.a11y.atspi.Registry", "/org/a11y/atspi/accessible/root", "org.a11y.atspi.Accessible",
                       "GetChildren", NULL, "(a(so))");
    if (r) {
      GVariantIter *it;
      const char *b, *p;
      g_variant_get(r, "(a(so))", &it);
      while (g_variant_iter_next(it, "(&s&o)", &b, &p)) {
        GVariant *n = prop(b, p, "org.a11y.atspi.Accessible", "Name");
        if (n && !strcmp(g_variant_get_string(n, NULL), argv[1])) snprintf(app_bus, sizeof app_bus, "%s", b);
        if (n) g_variant_unref(n);
      }
      g_variant_iter_free(it);
      g_variant_unref(r);
    }
    if (!*app_bus) wait(200);
  }
  if (!*app_bus) {
    FAIL("the registry does not list %s", argv[1]);
    return 1;
  }
  /* wait for the window */
  for (int tries = 0; tries < 50; tries++) {
    nobjs = 0;
    walk(app_bus, "/org/a11y/atspi/accessible/root", 0);
    if (nobjs > 3) break;
    wait(200);
  }
  Obj *app = &objs[0];
  if (app->role != 75) FAIL("the root's role is %u, not application", app->role);
  GVariant *tk = prop(app_bus, app->path, "org.a11y.atspi.Application", "ToolkitName");
  if (!tk || strcmp(g_variant_get_string(tk, NULL), "FLTK")) FAIL("toolkit name");
  if (tk) g_variant_unref(tk);
  find("A11y Test", 23);
  find("Apple", 32);
  find("Banana", 32); /* without the @b */

  /* a click */
  Obj *press = find("Press me", 43), *result = find("Result", 79);
  if (press && result) {
    GVariant *r = call(press->bus, press->path, "org.a11y.atspi.Action", "DoAction", g_variant_new("(i)", 0), "(b)");
    if (r) g_variant_unref(r);
    wait(800);
    char *t = text(result);
    if (strcmp(t, "pressed")) FAIL("Result says '%s' after the click", t);
    g_free(t);
  }
  /* text */
  Obj *name = find("Name", 79);
  if (name) {
    GVariant *r = call(name->bus, name->path, "org.a11y.atspi.EditableText", "SetTextContents", g_variant_new("(s)", "Ada"), "(b)");
    if (r) g_variant_unref(r);
    wait(600);
    char *t = text(name);
    if (strcmp(t, "Ada")) FAIL("Name holds '%s'", t);
    g_free(t);
    /* the focus, and its event */
    r = call(name->bus, name->path, "org.a11y.atspi.Component", "GrabFocus", NULL, "(b)");
    if (r) g_variant_unref(r);
    wait(1200);
    if (strcmp(focused_path, name->path)) FAIL("no focus event for Name (last: '%s')", focused_path);
    if (!(states(name) & (1ull << 12))) FAIL("Name is not focused");
  }
  /* a check box */
  Obj *wrap = find("Wrap lines", 7);
  if (wrap) {
    GVariant *r = call(wrap->bus, wrap->path, "org.a11y.atspi.Action", "DoAction", g_variant_new("(i)", 0), "(b)");
    if (r) g_variant_unref(r);
    wait(800);
    if (!(states(wrap) & (1ull << 4))) FAIL("Wrap lines is not checked");
  }
  /* a value */
  Obj *vol = find("Volume", 51);
  if (vol) {
    GVariant *r = call(vol->bus, vol->path, "org.freedesktop.DBus.Properties", "Set",
                       g_variant_new("(ssv)", "org.a11y.atspi.Value", "CurrentValue", g_variant_new_double(42)), NULL);
    if (r) g_variant_unref(r);
    wait(800);
    GVariant *v = prop(vol->bus, vol->path, "org.a11y.atspi.Value", "CurrentValue");
    if (!v || g_variant_get_double(v) != 42) FAIL("Volume is %g", v ? g_variant_get_double(v) : -1);
    if (v) g_variant_unref(v);
  }
  /* a list item */
  Obj *banana = find("Banana", 32);
  if (banana) {
    GVariant *r = call(banana->bus, banana->path, "org.a11y.atspi.Action", "DoAction", g_variant_new("(i)", 0), "(b)");
    if (r) g_variant_unref(r);
    wait(800);
    if (!(states(banana) & (1ull << 23))) FAIL("Banana is not selected");
  }
  /* menus: File › Save, reached and chosen like from the keyboard */
  find("File", 33);
  find("History", 8);
  Obj *save = find("Save", 35);
  if (save && result) {
    GVariant *r = call(save->bus, save->path, "org.a11y.atspi.Action", "DoAction", g_variant_new("(i)", 0), "(b)");
    if (r) g_variant_unref(r);
    wait(800);
    char *t = text(result);
    if (strcmp(t, "saved")) FAIL("Result says '%s' after File > Save", t);
    g_free(t);
  }
  printf("atspi-probe: %d objects, %d failures\n", nobjs, failures);
  return failures ? 1 : 0;
}
