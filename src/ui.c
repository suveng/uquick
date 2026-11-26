/* ui.c — 单窗口命令面板
 *
 * 性能要点：
 *  - 窗口与行控件只创建一次，搜索时只改文字/图标（无控件重建）；
 *  - 图标按名缓存 GIcon，避免每次查询重新查找主题；
 *  - 键盘事件走 CAPTURE 阶段统一处理，焦点始终留在输入框。
 */
#include "uquick.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>

#define ROW_ICON_PX 24

typedef struct {
  GtkWidget *row, *img, *title, *sub;
} UiRow;

static GtkWidget *win, *entry, *list, *box, *scroll;
static UiRow      rows[UQ_MAX_ROWS];
static UqResult   results[UQ_MAX_ROWS];
static int        n_results, sel;
static gboolean   ui_shown;
static GHashTable *icon_cache;

/* 动态项（计算结果 / 网址 / 路径 / shell）——每次查询重建，容量固定 */
static UqItem tr[4];
static int    n_tr;

static guint64 pending_us;
static guint64 last_latency_us;   /* 请求 -> 首帧上屏 */
static guint64 last_submit_us;    /* 请求 -> surface mapped（提交给合成器） */
static gint64  trace_fd = -1;

/* ---- 动态项 ---- */

static void transient_free(UqItem *it) {
  g_free(it->name); g_free(it->sub); g_free(it->key);
  g_free(it->match); g_free(it->path); g_free(it->arg); g_free(it->icon);
  memset(it, 0, sizeof(*it));
}

static void transients_clear(void) {
  for (int i = 0; i < n_tr; i++) transient_free(&tr[i]);
  n_tr = 0;
}

static UqItem *transient_new(UqKind kind, const char *name, const char *sub,
                             const char *icon, const char *arg) {
  if (n_tr >= (int)G_N_ELEMENTS(tr)) return NULL;
  UqItem *it = &tr[n_tr++];
  memset(it, 0, sizeof(*it));
  it->kind = kind;
  it->transient = TRUE;
  it->name = g_strdup(name);
  it->sub = g_strdup(sub);
  it->icon = g_strdup(icon);
  it->arg = g_strdup(arg);
  it->match = g_strdup(name ? name : "");
  for (char *p = it->match; *p; p++)
    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
  it->name_len = (guint16)strlen(it->match);
  return it;
}

static char *format_number(double v) {
  double r = round(v);
  if (fabs(v - r) < 1e-9 && fabs(v) < 1e15)
    return g_strdup_printf("%" G_GINT64_FORMAT, (gint64)r);
  return g_strdup_printf("%.10g", v);
}

static gboolean looks_like_url(const char *q) {
  return g_str_has_prefix(q, "http://") || g_str_has_prefix(q, "https://") ||
         g_str_has_prefix(q, "ftp://") || g_str_has_prefix(q, "www.") ||
         g_str_has_prefix(q, "file://");
}

static gboolean looks_like_math(const char *q) {
  gboolean digit = FALSE, op = FALSE;
  int depth = 0;
  for (const char *p = q; *p; p++) {
    if (g_ascii_isdigit(*p)) digit = TRUE;
    else if (*p == '(') depth++;
    else if (*p == ')') { if (--depth < 0) return FALSE; }
    else if (strchr("+-*/%^.", *p)) op = TRUE;
    else if (g_ascii_isalpha(*p) || *p == '_') { /* 函数名/常量 */ }
    else if (*p != ' ') return FALSE;
  }
  return digit && (op || (depth == 0 && strchr(q, '(') != NULL));
}

/* 构造动态项：> shell、= 计算、网址、路径、算式 */
static void build_transients(const char *q) {
  gboolean forced_calc = (q[0] == '=');

  if (!forced_calc && q[0] == '>') {
    const char *cmd = q + 1;
    while (*cmd == ' ') cmd++;
    if (!*cmd) return;
    char *nm = g_strdup_printf("运行：%s", cmd);
    transient_new(UQ_KIND_SHELL, nm, "在 shell 中执行", "utilities-terminal-symbolic", cmd);
    g_free(nm);
    return;
  }

  const char *expr = forced_calc ? q + 1 : q;
  double v = 0;
  if ((forced_calc || looks_like_math(expr)) && uq_calc_eval(expr, &v)) {
    char *num = format_number(v);
    transient_new(UQ_KIND_CALC, num, "回车复制结果", "accessories-calculator", num);
    g_free(num);
  }

  if (!forced_calc && looks_like_url(q)) {
    char *uri = g_strdup(q);
    if (g_str_has_prefix(q, "www.")) {
      g_free(uri);
      uri = g_strconcat("https://", q, NULL);
    }
    transient_new(UQ_KIND_URI, uri, "打开网址", "web-browser", uri);
    g_free(uri);
  }

  if (!forced_calc && (q[0] == '/' || g_str_has_prefix(q, "~/") || g_str_has_prefix(q, "./"))) {
    char *path = g_str_has_prefix(q, "~/")
        ? g_build_filename(g_get_home_dir(), q + 2, NULL)
        : g_strdup(q);
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri) {
      transient_new(UQ_KIND_PATH, path, "打开路径", "folder", uri);
      g_free(uri);
    }
    g_free(path);
  }
}

/* ---- 图标缓存 ---- */

static GIcon *icon_for(const char *name) {
  if (!name || !*name) name = "application-x-executable";
  GIcon *ic = g_hash_table_lookup(icon_cache, name);
  if (ic) return ic;
  if (name[0] == '/') {
    GFile *f = g_file_new_for_path(name);
    ic = g_file_icon_new(f);
    g_object_unref(f);
  } else {
    ic = g_themed_icon_new(name);
  }
  g_hash_table_insert(icon_cache, g_strdup(name), ic);
  return ic;
}

/* ---- 结果渲染 ---- */

static void render(void) {
  const char *q = gtk_editable_get_text(GTK_EDITABLE(entry));
  transients_clear();
  build_transients(q);

  n_results = uq_search(q, results, UQ_MAX_ROWS, tr, n_tr);

  for (int i = 0; i < UQ_MAX_ROWS; i++) {
    if (i >= n_results) {
      gtk_widget_set_visible(rows[i].row, FALSE);
      continue;
    }
    UqItem *it = results[i].item;
    gtk_label_set_text(GTK_LABEL(rows[i].title), it->name);
    gtk_widget_set_tooltip_text(rows[i].title, it->name);
    if (it->sub && *it->sub) {
      gtk_label_set_text(GTK_LABEL(rows[i].sub), it->sub);
      gtk_widget_set_visible(rows[i].sub, TRUE);
    } else {
      gtk_widget_set_visible(rows[i].sub, FALSE);
    }
    gtk_image_set_from_gicon(GTK_IMAGE(rows[i].img), icon_for(it->icon));
    gtk_widget_set_visible(rows[i].row, TRUE);
  }

  sel = n_results > 0 ? 0 : -1;
  if (sel >= 0)
    gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(rows[sel].row));
  else
    gtk_list_box_unselect_all(GTK_LIST_BOX(list));
}

static void activate_row(int i) {
  if (i < 0 || i >= n_results) return;
  uq_launch(results[i].item, GTK_WINDOW(win));
}

/* ---- 延迟测量 ---- */

static void trace_latency(guint64 us) {
  last_latency_us = us;
  if (!g_getenv("UQUICK_TRACE")) return;
  if (trace_fd < 0) {
    char *dir = g_build_filename(g_get_user_state_dir(), "uquick", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "latency.log", NULL);
    trace_fd = g_open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    g_free(dir);
    g_free(path);
  }
  if (trace_fd >= 0) {
    char buf[64];
    int n = g_snprintf(buf, sizeof(buf), "%" G_GUINT64_FORMAT "\n", us);
    if (write((int)trace_fd, buf, (size_t)n) < 0) { /* 忽略 */ }
  }
}

static void on_map(GtkWidget *w, gpointer data) {
  (void)data;
  if (!pending_us) return;
  guint64 now = (guint64)g_get_real_time();
  last_submit_us = now > pending_us ? now - pending_us : 0;
  g_signal_handlers_disconnect_by_func(w, on_map, NULL);
}

static void on_after_paint(GdkFrameClock *clock, gpointer data) {
  (void)data;
  if (!pending_us) return;
  guint64 now = (guint64)g_get_real_time();
  guint64 delta = now > pending_us ? now - pending_us : 0;
  pending_us = 0;
  trace_latency(delta);
  g_signal_handlers_disconnect_by_func(clock, on_after_paint, NULL);
}

/* ---- 事件 ---- */

static void move_sel(int delta) {
  if (n_results <= 0) return;
  sel += delta;
  if (sel < 0) sel = n_results - 1;
  if (sel >= n_results) sel = 0;
  gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(rows[sel].row));
}

static gboolean on_key(GtkEventControllerKey *ctl, guint keyval, guint keycode,
                       GdkModifierType state, gpointer data) {
  (void)ctl; (void)keycode; (void)state; (void)data;
  switch (keyval) {
    case GDK_KEY_Escape: {
      const char *t = gtk_editable_get_text(GTK_EDITABLE(entry));
      if (t && *t) {
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
        render();
      } else {
        uq_ui_hide();
      }
      return TRUE;
    }
    case GDK_KEY_Up:        move_sel(-1); return TRUE;
    case GDK_KEY_Down:      move_sel(1);  return TRUE;
    case GDK_KEY_Page_Up:   move_sel(-5); return TRUE;
    case GDK_KEY_Page_Down: move_sel(5);  return TRUE;
    case GDK_KEY_Tab:       move_sel(1);  return TRUE;
    case GDK_KEY_ISO_Left_Tab: move_sel(-1); return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      activate_row(sel);
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean on_close(GtkWindow *w, gpointer data) {
  (void)w; (void)data;
  uq_ui_hide();
  return TRUE;
}

static void on_active_changed(GObject *obj, GParamSpec *ps, gpointer data) {
  (void)obj; (void)ps; (void)data;
  if (!ui_shown) return;
  if (!gtk_window_is_active(GTK_WINDOW(win)))
    uq_ui_hide();
}

static void on_entry_changed(GtkEditable *ed, gpointer data) {
  (void)ed; (void)data;
  render();
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer data) {
  (void)lb; (void)data;
  activate_row(gtk_list_box_row_get_index(row));
}

/* ---- 构建 ---- */

static const char *css =
  "window.uqwin { background: transparent; }\n"
  ".uqbox { background-color: @theme_bg_color; border-radius: 16px;"
  "         box-shadow: 0 10px 34px rgba(0,0,0,0.30); }\n"
  "entry.uqentry, entry.uqentry:focus, entry.uqentry:focus-within,"
  "entry.uqentry:focus-visible { background: transparent; border: none;"
  "  box-shadow: none; outline: none; font-size: 17px; padding: 15px 18px; }\n"
  "separator.uqsep { background-color: alpha(@theme_fg_color, 0.12);"
  "  min-height: 1px; margin: 0 12px; }\n"
  ".uqlist { background: transparent; padding: 6px; }\n"
  ".uqlist row { border-radius: 10px; padding: 7px 10px; }\n"
  ".uqlist row:selected { background-color: @theme_selected_bg_color; }\n"
  ".uqlist row:selected label { color: @theme_selected_fg_color; }\n"
  ".uqlist row:selected label.uqsub { opacity: 0.8; }\n"
  "label.uqtitle { font-size: 14.5px; }\n"
  "label.uqsub { font-size: 12px; opacity: 0.55; }\n";

static void build_rows(void) {
  for (int i = 0; i < UQ_MAX_ROWS; i++) {
    UiRow *r = &rows[i];
    r->row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r->row), TRUE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(r->row), TRUE);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    r->img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(r->img), ROW_ICON_PX);
    r->title = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(r->title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(r->title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(r->title), 48);
    gtk_widget_set_hexpand(r->title, TRUE);
    gtk_widget_add_css_class(r->title, "uqtitle");
    r->sub = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(r->sub), 1.0f);
    gtk_label_set_ellipsize(GTK_LABEL(r->sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(r->sub), 30);
    gtk_widget_add_css_class(r->sub, "uqsub");
    gtk_box_append(GTK_BOX(hb), r->img);
    gtk_box_append(GTK_BOX(hb), r->title);
    gtk_box_append(GTK_BOX(hb), r->sub);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r->row), hb);
    gtk_list_box_append(GTK_LIST_BOX(list), r->row);
    gtk_widget_set_visible(r->row, FALSE);
  }
}

void uq_ui_init(void) {
  icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);

  win = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(win), "UQuick");
  gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(win), 760, -1);
  gtk_widget_add_css_class(win, "uqwin");

  /* 窗口创建后 GDK display 才可用 */
  GdkDisplay *display = gdk_display_get_default();
  if (display) {
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
  }

  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(box, "uqbox");
  gtk_widget_set_margin_start(box, 0);
  gtk_window_set_child(GTK_WINDOW(win), box);

  entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "搜索应用、计算、网址、路径，> 执行命令");
  gtk_widget_add_css_class(entry, "uqentry");
  gtk_box_append(GTK_BOX(box), entry);

  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(sep, "uqsep");
  gtk_box_append(GTK_BOX(box), sep);

  list = gtk_list_box_new();
  gtk_widget_add_css_class(list, "uqlist");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
  gtk_list_box_set_show_separators(GTK_LIST_BOX(list), FALSE);

  scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
  gtk_box_append(GTK_BOX(box), scroll);

  build_rows();

  g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), NULL);
  g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
  g_signal_connect(win, "close-request", G_CALLBACK(on_close), NULL);
  g_signal_connect(win, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

  GtkEventController *kc = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
  g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), NULL);
  gtk_widget_add_controller(win, kc);
}

void uq_ui_set_pending(guint64 unix_us) { pending_us = unix_us; }

void uq_ui_show(void) {
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
  render();
  if (pending_us) {
    g_signal_handlers_disconnect_by_func(win, on_map, NULL);
    g_signal_connect(win, "map", G_CALLBACK(on_map), NULL);
  }
  gtk_window_present(GTK_WINDOW(win));
  gtk_editable_set_position(GTK_EDITABLE(entry), 0);
  gtk_widget_grab_focus(entry);
  ui_shown = TRUE;

  GdkFrameClock *clock = gtk_widget_get_frame_clock(win);
  if (clock && pending_us) {
    g_signal_handlers_disconnect_by_func(clock, on_after_paint, NULL);
    g_signal_connect(clock, "after-paint", G_CALLBACK(on_after_paint), NULL);
  }
}

void uq_ui_set_query(const char *text) {
  gtk_editable_set_text(GTK_EDITABLE(entry), text ? text : "");
  gtk_editable_set_position(GTK_EDITABLE(entry), -1);
  render();
}

/* 守护进程起来后先空跑一次「映射→首帧→隐藏」，把 GTK 渲染管线与
 * 首块 buffer 的分配成本挪到登录时；用 set_visible 而不是 present，
 * 这样不会向合成器申请激活、不抢焦点。 */
static gboolean warmup_done(gpointer data) {
  (void)data;
  gtk_widget_set_visible(win, FALSE);
  gtk_widget_set_opacity(win, 1.0);
  return G_SOURCE_REMOVE;
}

void uq_ui_warmup(void) {
  gtk_widget_set_opacity(win, 0.0);
  gtk_widget_set_visible(win, TRUE);
  g_timeout_add(120, warmup_done, NULL);
}

void uq_ui_hide(void) {
  ui_shown = FALSE;
  gtk_widget_set_visible(win, FALSE);
}

gboolean uq_ui_is_visible(void) {
  return ui_shown && gtk_widget_get_visible(win);
}

void uq_ui_toggle(void) {
  if (uq_ui_is_visible()) uq_ui_hide();
  else uq_ui_show();
}

void uq_ui_run(void) {
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);
}

/* ---- 延迟基准：显示 20 次，报告“按下热键到首帧上屏” ---- */

static int      bench_left;
static guint64  bench_sum, bench_min, bench_max;
static guint64  bench_sub_sum, bench_sub_min, bench_sub_max;
static guint64  bench_start;

static gboolean bench_show(gpointer data) {
  (void)data;
  pending_us = (guint64)g_get_real_time();
  uq_ui_show();
  return G_SOURCE_REMOVE;
}

static gboolean bench_tick(gpointer data) {
  (void)data;
  if (pending_us) return G_SOURCE_CONTINUE;
  if (bench_left > 0) {
    if (last_latency_us < bench_min) bench_min = last_latency_us;
    if (last_latency_us > bench_max) bench_max = last_latency_us;
    bench_sum += last_latency_us;
    if (last_submit_us < bench_sub_min) bench_sub_min = last_submit_us;
    if (last_submit_us > bench_sub_max) bench_sub_max = last_submit_us;
    bench_sub_sum += last_submit_us;
    bench_left--;
    uq_ui_hide();
    g_idle_add(bench_show, NULL);
    return G_SOURCE_CONTINUE;
  }
  guint64 total = (guint64)(g_get_monotonic_time() - (gint64)bench_start);
  printf("请求 -> surface 提交 : avg=%.2f ms  min=%.2f  max=%.2f\n",
         (double)bench_sub_sum / 20.0 / 1000.0, (double)bench_sub_min / 1000.0,
         (double)bench_sub_max / 1000.0);
  printf("请求 -> 首帧上屏     : avg=%.2f ms  min=%.2f  max=%.2f  (20 次往返 %.1f ms)\n",
         (double)bench_sum / 20.0 / 1000.0, (double)bench_min / 1000.0,
         (double)bench_max / 1000.0, (double)total / 1000.0);
  fflush(stdout);
  exit(0);
  return G_SOURCE_REMOVE;
}

void uq_ui_bench(void) {
  bench_left = 20;
  bench_min = bench_sub_min = G_MAXUINT64;
  bench_sum = bench_max = bench_sub_sum = bench_sub_max = 0;
  bench_start = (guint64)g_get_monotonic_time();
  g_idle_add(bench_show, NULL);
  g_timeout_add(4, bench_tick, NULL);
}
