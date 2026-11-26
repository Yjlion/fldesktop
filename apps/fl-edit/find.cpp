// fl-edit: find, replace and go to line
#include "edit_app.h"

// ---- find / replace -------------------------------------------------------------------------

struct FindDialog {
  Fl_Double_Window *win = nullptr;
  Fl_Input *find = nullptr, *repl = nullptr;
  Fl_Check_Button *match_case = nullptr, *wrap = nullptr;
  Fl_Box *msg = nullptr;
} F;

bool find_next(bool backwards) {
  Doc *d = current();
  if (!d || !F.find || !*F.find->value()) return false;
  const char *needle = F.find->value();
  bool mc = F.match_case->value();
  int start = d->ed->insert_position(), found;
  bool ok;
  if (backwards) {
    int a, b;
    if (d->buf->selection_position(&a, &b)) start = a;
    ok = start > 0 && d->buf->search_backward(start - 1, needle, &found, mc);
    if (!ok && F.wrap->value()) ok = d->buf->search_backward(d->buf->length(), needle, &found, mc);
  } else {
    ok = d->buf->search_forward(start, needle, &found, mc);
    if (!ok && F.wrap->value()) ok = d->buf->search_forward(0, needle, &found, mc);
  }
  if (!ok) {
    F.msg->label("Not found");
    fl_beep();
    return false;
  }
  F.msg->label("");
  int len = (int)strlen(needle);
  d->buf->select(found, found + len);
  d->ed->insert_position(backwards ? found : found + len);
  d->ed->show_insert_position();
  update_status();
  return true;
}

static void replace_one() {
  Doc *d = current();
  if (!d || !*F.find->value()) return;
  int a, b;
  if (d->buf->selection_position(&a, &b)) {
    char *sel = d->buf->selection_text();
    bool match = F.match_case->value() ? strcmp(sel, F.find->value()) == 0 : strcasecmp(sel, F.find->value()) == 0;
    free(sel);
    if (match) {
      d->buf->replace(a, b, F.repl->value());
      d->ed->insert_position(a + (int)strlen(F.repl->value()));
    }
  }
  find_next();
}

static void replace_all() {
  Doc *d = current();
  if (!d || !*F.find->value()) return;
  const char *needle = F.find->value(), *rep = F.repl->value();
  int pos = 0, found, count = 0, nl = (int)strlen(needle), rl = (int)strlen(rep);
  while (d->buf->search_forward(pos, needle, &found, F.match_case->value())) {
    d->buf->replace(found, found + nl, rep);
    pos = found + rl;
    count++;
  }
  static char m[64];
  snprintf(m, sizeof m, "Replaced %d occurrence%s", count, count == 1 ? "" : "s");
  F.msg->label(m);
}

void show_find(bool replace) {
  if (!F.win) {
    F.win = new Fl_Double_Window(440, 160, "Find and Replace");
    F.find = new Fl_Input(90, 12, 230, 28, "Find:");
    F.repl = new Fl_Input(90, 46, 230, 28, "Replace:");
    F.match_case = new fld::CheckButton(90, 80, 120, 24, "Match case");
    F.wrap = new fld::CheckButton(210, 80, 120, 24, "Wrap around");
    F.wrap->value(1);
    Fl_Return_Button *next = new Fl_Return_Button(330, 12, 100, 28, "Next");
    next->callback([](Fl_Widget *, void *) { find_next(); });
    Fl_Button *prev = new Fl_Button(330, 46, 100, 28, "Previous");
    prev->callback([](Fl_Widget *, void *) { find_next(true); });
    Fl_Button *r1 = new Fl_Button(220, 120, 100, 28, "Replace");
    r1->callback([](Fl_Widget *, void *) { replace_one(); });
    Fl_Button *ra = new Fl_Button(330, 120, 100, 28, "Replace All");
    ra->callback([](Fl_Widget *, void *) { replace_all(); });
    F.msg = new Fl_Box(10, 120, 200, 28);
    F.msg->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    F.msg->labelsize(12);
    F.win->end();
    F.win->set_non_modal();
  }
  Doc *d = current();
  if (d && d->buf->selected()) {
    char *sel = d->buf->selection_text();
    if (!strchr(sel, '\n')) F.find->value(sel);
    free(sel);
  }
  F.win->show();
  (replace && *F.find->value() ? F.repl : F.find)->take_focus();
}

void goto_line() {
  Doc *d = current();
  if (!d) return;
  const char *v = fl_input("Go to line:", "");
  if (!v) return;
  int line = atoi(v);
  if (line < 1) return;
  int pos = d->buf->skip_lines(0, line - 1);
  d->ed->insert_position(pos);
  d->ed->show_insert_position();
  d->ed->take_focus();
  update_status();
}
