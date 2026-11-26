/* launch.c — 执行动作：应用、系统命令、用户命令、计算复制、URI/路径 */
#include "uquick.h"

#include <string.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixinputstream.h>

/* 桌面项里没有的场景：直接 fork（stdout/stderr 静默，避免管道悬挂） */
static void spawn_shell(const char *cmd) {
  if (!cmd || !*cmd) return;
  char *argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };
  g_spawn_async(NULL, argv, NULL,
                G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                G_SPAWN_STDERR_TO_DEV_NULL,
                NULL, NULL, NULL, NULL);
}

/* 去掉 Exec 里的 %f %U 等字段码，作为 g_app_info_launch 失败时的回退 */
static char *strip_field_codes(const char *exec) {
  GString *g = g_string_sized_new(strlen(exec) + 1);
  for (const char *p = exec; *p; p++) {
    if (*p == '%' && p[1]) {
      char c = p[1];
      if (c == '%') { g_string_append_c(g, '%'); p++; continue; }
      if (strchr("fFuUikcdDnNvm", c)) { p++; continue; }
      continue; /* 未知字段码整段丢弃 */
    }
    g_string_append_c(g, *p);
  }
  return g_string_free(g, FALSE);
}

static void uri_done(GObject *src, GAsyncResult *res, gpointer data) {
  (void)data;
  GError *err = NULL;
  gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(src), res, &err);
  g_clear_error(&err);
  g_object_unref(src);
}

static void launch_app(UqItem *it, GtkWidget *win) {
  GDesktopAppInfo *info = it->path ? g_desktop_app_info_new_from_filename(it->path) : NULL;
  if (info) {
    GdkDisplay *display = win ? gtk_widget_get_display(win) : gdk_display_get_default();
    GdkAppLaunchContext *ctx = display ? gdk_display_get_app_launch_context(display) : NULL;
    GError *err = NULL;
    gboolean ok = g_app_info_launch(G_APP_INFO(info), NULL,
                                    ctx ? G_APP_LAUNCH_CONTEXT(ctx) : NULL, &err);
    if (!ok) {
      if (ctx) {
        /* 没有启动通知上下文时再试一次（部分环境上下文会失败） */
        g_clear_error(&err);
        ok = g_app_info_launch(G_APP_INFO(info), NULL, NULL, &err);
      }
    }
    if (!ok) {
      g_clear_error(&err);
      char *cmd = strip_field_codes(it->arg ? it->arg : "");
      spawn_shell(cmd);
      g_free(cmd);
    }
    if (ctx) g_object_unref(ctx);
    g_object_unref(info);
    return;
  }
  char *cmd = strip_field_codes(it->arg ? it->arg : "");
  spawn_shell(cmd);
  g_free(cmd);
}

void uq_copy_text(const char *text) {
  if (!text) return;
  GdkDisplay *display = gdk_display_get_default();
  if (!display) return;
  GdkClipboard *cb = gdk_display_get_clipboard(display);
  gdk_clipboard_set_text(cb, text);
}

void uq_launch(UqItem *it, GtkWindow *win) {
  if (!it) return;
  if (it->key) uq_frec_bump(it->key);

  switch (it->kind) {
    case UQ_KIND_CALC:
      uq_copy_text(it->arg);
      uq_ui_hide();
      break;
    case UQ_KIND_URI:
    case UQ_KIND_PATH: {
      GtkUriLauncher *l = gtk_uri_launcher_new(it->arg);
      gtk_uri_launcher_launch(l, win, NULL, uri_done, NULL);
      uq_ui_hide();
      break;
    }
    case UQ_KIND_APP:
      launch_app(it, win ? GTK_WIDGET(win) : NULL);
      uq_ui_hide();
      break;
    case UQ_KIND_ACTION:
    case UQ_KIND_COMMAND:
    case UQ_KIND_SHELL:
    default:
      spawn_shell(it->arg);
      uq_ui_hide();
      break;
  }
}
