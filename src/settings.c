/* settings.c — 设置窗口（GTK4，纯代码构建，无 libadwaita 依赖）
 *
 * 内容：开机自启开关、索引状态与重建、当前热键、自定义命令文件、退出守护进程。
 * 入口：托盘菜单「设置…」/ `uquick --settings` / IPC 命令 settings。
 */
#include "uquick.h"

#include <gio/gio.h>
#include <string.h>

static GtkWidget *win;
static GtkWidget *sw_autostart;
static GtkWidget *lbl_index;
static GtkWidget *lbl_hotkey;
static gboolean   updating;

static GtkWidget *row_new(const char *title, const char *sub, GtkWidget *trailing) {
  GtkWidget *row = gtk_list_box_row_new();
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(v, TRUE);
  gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
  GtkWidget *t = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
  gtk_box_append(GTK_BOX(v), t);
  if (sub && *sub) {
    GtkWidget *s = gtk_label_new(sub);
    gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(s), 42);
    gtk_widget_add_css_class(s, "uqsub");
    gtk_box_append(GTK_BOX(v), s);
  }
  gtk_box_append(GTK_BOX(box), v);
  if (trailing) {
    gtk_widget_set_valign(trailing, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), trailing);
  }
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  return row;
}

/* 从 gsettings 里读出我们这条自定义快捷键的组合 */
static char *current_hotkey(void) {
  GSettings *mk = g_settings_new("org.gnome.settings-daemon.plugins.media-keys");
  char **paths = g_settings_get_strv(mk, "custom-keybindings");
  char *found = NULL;
  for (char **p = paths; *p && !found; p++) {
    GSettings *cs = g_settings_new_with_path(
        "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding", *p);
    char *cmd = g_settings_get_string(cs, "command");
    if (cmd && strstr(cmd, "uquick")) {
      char *b = g_settings_get_string(cs, "binding");
      found = g_strdup(b);
      g_free(b);
    }
    g_free(cmd);
    g_object_unref(cs);
  }
  g_strfreev(paths);
  g_object_unref(mk);
  return found;
}

static void refresh_labels(void) {
  char *hk = current_hotkey();
  gtk_label_set_text(GTK_LABEL(lbl_hotkey), hk ? hk : "未设置（运行 contrib/install-hotkey.sh）");
  g_free(hk);

  char *idx = g_strdup_printf("%u 项 · 扫描 %.1f ms", uq_index_count(),
                              (double)uq_index_scan_us() / 1000.0);
  gtk_label_set_text(GTK_LABEL(lbl_index), idx);
  g_free(idx);

  updating = TRUE;
  gtk_switch_set_active(GTK_SWITCH(sw_autostart), uq_autostart_enabled());
  updating = FALSE;
}

static void on_autostart(GtkSwitch *sw, GParamSpec *ps, gpointer data) {
  (void)ps; (void)data;
  if (updating) return;
  GError *err = NULL;
  if (!uq_autostart_set(gtk_switch_get_active(sw), &err)) {
    g_printerr("uquick: 切换开机自启失败: %s\n", err ? err->message : "未知");
    g_clear_error(&err);
    refresh_labels();
    return;
  }
  uq_index_load();        /* 面板里的「开机自启」副标题同步 */
  if (uq_ui_is_visible()) uq_ui_refresh();
  uq_tray_update();
  refresh_labels();
}

static void on_rescan(GtkButton *b, gpointer data) {
  (void)b; (void)data;
  uq_index_load();
  if (uq_ui_is_visible()) uq_ui_refresh();
  refresh_labels();
}

static void on_open_commands(GtkButton *b, gpointer data) {
  (void)b; (void)data;
  char *dir = g_build_filename(g_get_user_config_dir(), "uquick", NULL);
  g_mkdir_with_parents(dir, 0700);
  char *path = g_build_filename(dir, "commands.tsv", NULL);
  if (!g_file_test(path, G_FILE_TEST_EXISTS))
    g_file_set_contents(path,
        "# 自定义命令：每行  名称<TAB>命令[<TAB>关键词]\n"
        "# 改完执行 uquick --reload 生效\n"
        "# 例：开代理\t~/.local/bin/proxy-on\tproxy vpn\n", -1, NULL);
  char *uri = g_filename_to_uri(path, NULL, NULL);
  if (uri) {
    GtkUriLauncher *l = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(l, GTK_WINDOW(win), NULL, NULL, NULL);
    g_object_unref(l);
    g_free(uri);
  }
  g_free(path);
  g_free(dir);
}

static void on_quit(GtkButton *b, gpointer data) {
  (void)b; (void)data;
  uq_quit();
}

static void on_close(GtkWindow *w, gpointer data) {
  (void)data;
  gtk_window_destroy(w);
  win = NULL;
}

void uq_settings_show(void) {
  if (win) {
    gtk_window_present(GTK_WINDOW(win));
    refresh_labels();
    return;
  }

  win = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(win), "UQuick 设置");
  gtk_window_set_default_size(GTK_WINDOW(win), 560, -1);
  gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

  GtkWidget *hb = gtk_header_bar_new();
  gtk_window_set_titlebar(GTK_WINDOW(win), hb);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(win), root);

  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_set_margin_top(list, 6);
  gtk_widget_set_margin_bottom(list, 6);
  gtk_box_append(GTK_BOX(root), list);

  /* 开机自启 */
  sw_autostart = gtk_switch_new();
  gtk_widget_set_valign(sw_autostart, GTK_ALIGN_CENTER);
  g_signal_connect(sw_autostart, "notify::active", G_CALLBACK(on_autostart), NULL);
  gtk_list_box_append(GTK_LIST_BOX(list),
      row_new("开机自启", "登录桌面时启动常驻进程，热键才能真正秒开", sw_autostart));

  /* 索引 */
  lbl_index = gtk_label_new(NULL);
  gtk_widget_add_css_class(lbl_index, "uqsub");
  GtkWidget *btn_rescan = gtk_button_new_with_label("重新扫描");
  g_signal_connect(btn_rescan, "clicked", G_CALLBACK(on_rescan), NULL);
  GtkWidget *box_idx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_append(GTK_BOX(box_idx), lbl_index);
  gtk_box_append(GTK_BOX(box_idx), btn_rescan);
  gtk_list_box_append(GTK_LIST_BOX(list), row_new("应用索引", NULL, box_idx));

  /* 热键 */
  lbl_hotkey = gtk_label_new(NULL);
  gtk_label_set_selectable(GTK_LABEL(lbl_hotkey), TRUE);
  gtk_widget_add_css_class(lbl_hotkey, "uqsub");
  gtk_list_box_append(GTK_LIST_BOX(list), row_new("唤出热键", NULL, lbl_hotkey));

  /* 自定义命令 */
  GtkWidget *btn_cmd = gtk_button_new_with_label("打开");
  g_signal_connect(btn_cmd, "clicked", G_CALLBACK(on_open_commands), NULL);
  gtk_list_box_append(GTK_LIST_BOX(list),
      row_new("自定义命令", "~/.config/uquick/commands.tsv（名称<TAB>命令）", btn_cmd));

  /* 底栏 */
  GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(footer, 12);
  gtk_widget_set_margin_end(footer, 12);
  gtk_widget_set_margin_bottom(footer, 12);
  GtkWidget *ver = gtk_label_new("UQuick " UQ_VERSION);
  gtk_label_set_xalign(GTK_LABEL(ver), 0.0f);
  gtk_widget_add_css_class(ver, "uqsub");
  gtk_widget_set_hexpand(ver, TRUE);
  GtkWidget *btn_quit = gtk_button_new_with_label("退出守护进程");
  g_signal_connect(btn_quit, "clicked", G_CALLBACK(on_quit), NULL);
  gtk_box_append(GTK_BOX(footer), ver);
  gtk_box_append(GTK_BOX(footer), btn_quit);
  gtk_box_append(GTK_BOX(root), footer);

  g_signal_connect(win, "close-request", G_CALLBACK(on_close), NULL);

  refresh_labels();
  gtk_window_present(GTK_WINDOW(win));
}

gboolean uq_settings_is_open(void) { return win != NULL; }
