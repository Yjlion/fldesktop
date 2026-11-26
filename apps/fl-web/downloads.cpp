// fl-web: downloads
#include "web_app.h"

// ---------------------------------------------------------------------------
// Downloads

static Fl_Double_Window *g_dl_win;
static Fl_Hold_Browser *g_dl_list;

static std::string download_line(const Download &d) {
  std::string state;
  if (d.failed) state = "Failed: " + d.error;
  else if (d.done) state = "Done";
  else {
    guint64 got = webkit_download_get_received_data_length(d.dl);
    guint64 total = 0;
    if (WebKitURIResponse *r = webkit_download_get_response(d.dl)) total = webkit_uri_response_get_content_length(r);
    state = fld::human_size(got) + (total ? " of " + fld::human_size(total) : "");
  }
  return fld::escape_at(d.name) + "\t" + fld::escape_at(state);
}

static void refresh_downloads(void * = nullptr) {
  if (!g_dl_win || !g_dl_win->shown()) return;
  int sel = g_dl_list->value();
  g_dl_list->clear();
  for (auto it = g_downloads.rbegin(); it != g_downloads.rend(); ++it) g_dl_list->add(download_line(**it).c_str());
  if (sel > 0 && sel <= g_dl_list->size()) g_dl_list->value(sel);
  bool running = std::any_of(g_downloads.begin(), g_downloads.end(), [](auto &d) { return !d->done && !d->failed; });
  if (running) Fl::repeat_timeout(0.5, refresh_downloads);
}

static Download *selected_download() {
  int line = g_dl_list ? g_dl_list->value() : 0;
  if (line <= 0 || line > (int)g_downloads.size()) return nullptr;
  return g_downloads[g_downloads.size() - (size_t)line].get();
}

void show_downloads() {
  if (!g_dl_win) {
    g_dl_win = new Fl_Double_Window(560, 340, "Downloads");
    g_dl_list = new Fl_Hold_Browser(10, 10, 540, 280);
    static int cols[] = {360, 0};
    g_dl_list->column_widths(cols);
    g_dl_list->column_char('\t');
    g_dl_list->callback([](Fl_Widget *, void *) {
      if (Fl::event_clicks())
        if (Download *d = selected_download())
          if (d->done) fld::open_path(d->path);
    });
    Fl_Button *open = new Fl_Button(10, 300, 100, 30, "Open");
    open->callback([](Fl_Widget *, void *) {
      if (Download *d = selected_download()) if (d->done) fld::open_path(d->path);
    });
    Fl_Button *folder = new Fl_Button(116, 300, 120, 30, "Show in Folder");
    folder->callback([](Fl_Widget *, void *) {
      Download *d = selected_download();
      fld::spawn({"fl-files", d ? fld::dirname(d->path) : fld::user_dir("DOWNLOAD")});
    });
    Fl_Button *cancel = new Fl_Button(242, 300, 100, 30, "Cancel");
    cancel->callback([](Fl_Widget *, void *) {
      if (Download *d = selected_download()) if (!d->done && !d->failed) webkit_download_cancel(d->dl);
      glib_loop_poke();
    });
    Fl_Button *clear = new Fl_Button(450, 300, 100, 30, "Clear List");
    clear->callback([](Fl_Widget *, void *) {
      g_downloads.erase(std::remove_if(g_downloads.begin(), g_downloads.end(), [](auto &d) {
        if (d->done || d->failed) { g_object_unref(d->dl); return true; }
        return false;
      }), g_downloads.end());
      refresh_downloads();
    });
    g_dl_win->resizable(g_dl_list);
    g_dl_win->end();
  }
  g_dl_win->show();
  Fl::remove_timeout(refresh_downloads);
  Fl::add_timeout(0, refresh_downloads);
}

void download_started(WebKitNetworkSession *, WebKitDownload *dl, gpointer) {
  auto d = std::make_unique<Download>();
  d->dl = WEBKIT_DOWNLOAD(g_object_ref(dl));
  Download *dp = d.get();
  g_downloads.push_back(std::move(d));
  g_signal_connect(dl, "decide-destination", CB([](WebKitDownload *dl, char *suggested, gpointer d) -> gboolean {
    Download *dp = (Download *)d;
    std::string name = fld::basename(str(suggested));
    if (name.empty() || name == "/" || name == "." || name == "..") name = "download";
    std::string dir = fld::user_dir("DOWNLOAD");
    if (dir.empty()) dir = fld::home_dir();
    fld::mkdir_p(dir);
    dp->path = fld::unique_name(dir, name);
    dp->name = fld::basename(dp->path);
    webkit_download_set_destination(dl, dp->path.c_str());
    return TRUE;
  }), dp);
  g_signal_connect(dl, "finished", CB([](WebKitDownload *, gpointer d) {
    Download *dp = (Download *)d;
    if (dp->failed) return;
    dp->done = true;
    fld::Notification n;
    n.app_name = "Web";
    n.summary = "Download finished";
    n.body = dp->name;
    n.icon = "folder-download";
    n.category = "transfer.complete";
    n.actions = {{"default", "Open"}, {"folder", "Show in Folder"}};
    std::string path = dp->path;
    fld::notify(n, [path](const std::string &a) {
      if (a == "folder") fld::spawn({"fl-files", fld::dirname(path)});
      else fld::open_path(path);
    });
    refresh_downloads();
  }), dp);
  g_signal_connect(dl, "failed", CB([](WebKitDownload *, GError *e, gpointer d) {
    Download *dp = (Download *)d;
    dp->failed = true;
    dp->error = e && e->code == WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER ? "cancelled" : e ? e->message : "?";
    refresh_downloads();
  }), dp);
  later([] { show_downloads(); });
}
