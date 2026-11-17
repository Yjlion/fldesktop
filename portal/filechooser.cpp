// org.freedesktop.impl.portal.FileChooser: OpenFile, SaveFile, SaveFiles
#include <FL/Fl_Double_Window.H>

#include <memory>

#include "fld/glib_bridge.h"
#include "fld/util.h"
#include "portal.h"

namespace portal {

void filechooser_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  const char *handle, *app_id, *parent, *title;
  GVariant *options;
  g_variant_get(params, "(&o&s&s&s@a{sv})", &handle, &app_id, &parent, &title, &options);

  fld::FileDialogOptions o;
  o.title = title;
  if (method == "OpenFile") o.mode = fld::FileDialogOptions::OPEN;
  else if (method == "SaveFile") o.mode = fld::FileDialogOptions::SAVE;
  else if (method == "SaveFiles") o.mode = fld::FileDialogOptions::SAVE_FILES;
  else {
    g_variant_unref(options);
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method.c_str());
    return;
  }
  o.accept_label = opt_string(options, "accept_label");
  o.multiple = opt_bool(options, "multiple");
  o.directory = opt_bool(options, "directory");
  if (GVariant *f = g_variant_lookup_value(options, "filters", G_VARIANT_TYPE("a(sa(us))"))) {
    o.filters = parse_filters(f);
    g_variant_unref(f);
  }
  if (GVariant *cf = g_variant_lookup_value(options, "current_filter", G_VARIANT_TYPE("(sa(us))"))) {
    fld::FileFilter cur = parse_filter(cf);
    g_variant_unref(cf);
    // the current filter must be one of the filters; add it when there are none
    o.current_filter = -1;
    for (size_t i = 0; i < o.filters.size(); i++)
      if (o.filters[i].name == cur.name) o.current_filter = (int)i;
    if (o.current_filter < 0) {
      o.filters.insert(o.filters.begin(), cur);
      o.current_filter = 0;
    }
  }
  if (GVariant *c = g_variant_lookup_value(options, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"))) {
    o.choices = parse_choices(c);
    g_variant_unref(c);
  }
  if (GVariant *v = g_variant_lookup_value(options, "current_folder", G_VARIANT_TYPE_BYTESTRING)) {
    o.folder = bytestring(v);
    g_variant_unref(v);
  }
  o.name = opt_string(options, "current_name");
  if (GVariant *v = g_variant_lookup_value(options, "current_file", G_VARIANT_TYPE_BYTESTRING)) {
    std::string f = bytestring(v);
    g_variant_unref(v);
    if (!f.empty()) {
      o.folder = fld::dirname(f);
      o.name = fld::basename(f);
    }
  }
  if (GVariant *v = g_variant_lookup_value(options, "files", G_VARIANT_TYPE("aay"))) {
    GVariantIter it;
    g_variant_iter_init(&it, v);
    GVariant *child;
    while ((child = g_variant_iter_next_value(&it))) {
      o.files.push_back(fld::basename(bytestring(child)));
      g_variant_unref(child);
    }
    g_variant_unref(v);
  }
  g_variant_unref(options);

  std::string h = handle;
  bool save_files = o.mode == fld::FileDialogOptions::SAVE_FILES;
  std::vector<std::string> files = o.files;
  fld::ui_call([o, h, inv, save_files, files] {
    auto filters = o.filters;
    auto alive = std::make_shared<bool>(true);
    Fl_Double_Window *dlg = fld::show_file_dialog(o, [h, inv, filters, save_files, files, alive](const fld::FileDialogResult &r) {
      *alive = false;
      request_end(h);
      if (!r.ok) {
        reply(inv, CANCELLED);
        return;
      }
      GVariantBuilder res;
      g_variant_builder_init(&res, G_VARIANT_TYPE("a{sv}"));
      GVariantBuilder uris;
      g_variant_builder_init(&uris, G_VARIANT_TYPE("as"));
      if (save_files) {
        // one uri per file to write, inside the chosen folder
        for (auto &f : files) g_variant_builder_add(&uris, "s", path_to_uri(fld::path_join(r.paths[0], f)).c_str());
      } else {
        for (auto &p : r.paths) g_variant_builder_add(&uris, "s", path_to_uri(p).c_str());
      }
      g_variant_builder_add(&res, "{sv}", "uris", g_variant_builder_end(&uris));
      if (!r.choices.empty()) {
        GVariantBuilder ch;
        g_variant_builder_init(&ch, G_VARIANT_TYPE("a(ss)"));
        for (auto &c : r.choices) g_variant_builder_add(&ch, "(ss)", c.first.c_str(), c.second.c_str());
        g_variant_builder_add(&res, "{sv}", "choices", g_variant_builder_end(&ch));
      }
      if (r.filter >= 0 && r.filter < (int)filters.size())
        g_variant_builder_add(&res, "{sv}", "current_filter", filter_to_variant(filters[r.filter]));
      g_variant_builder_add(&res, "{sv}", "writable", g_variant_new_boolean(TRUE));
      reply(inv, SUCCESS, g_variant_builder_end(&res));
    });
    request_begin(h, [dlg, alive] {
      if (*alive) fld::cancel_file_dialog(dlg);
    });
  });
}

} // namespace portal
