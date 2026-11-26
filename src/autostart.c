/* autostart.c — 开机自启开关（XDG autostart，用户级）
 *
 * 生效位置：$XDG_CONFIG_HOME/autostart/uquick-daemon.desktop
 * 若 /etc/xdg/autostart 里也有同名文件（deb 安装带的自启），
 * 用同名的用户级文件 + Hidden=true 屏蔽它 —— 这是 XDG 规范里
 * 用户级覆盖系统级的正规做法，不需要 root。
 */
#include "uquick.h"

#include <glib/gstdio.h>
#include <string.h>

#define AUTOSTART_NAME "uquick-daemon.desktop"

static const char *user_path(void) {
  static char *p;
  if (!p) p = g_build_filename(g_get_user_config_dir(), "autostart", AUTOSTART_NAME, NULL);
  return p;
}

static const char *system_path(void) {
  static char *p;
  if (!p) {
    const char *const *dirs = g_get_system_config_dirs();   /* 通常是 /etc/xdg */
    p = g_build_filename(dirs && dirs[0] ? dirs[0] : "/etc/xdg", "autostart",
                         AUTOSTART_NAME, NULL);
  }
  return p;
}

const char *uq_autostart_exec(void) {
  static char *p;
  if (p) return p;
  const char *inst = g_getenv("SNAP_INSTANCE_NAME");
  if (inst && *inst) {
    /* snap 里必须用稳定的入口，不能用 /snap/<name>/xN/usr/bin/...（换 revision 就失效） */
    p = g_strdup_printf("/snap/bin/%s", inst);
  } else {
    char *self = g_file_read_link("/proc/self/exe", NULL);
    p = self ? self : g_strdup("uquick");
  }
  return p;
}

/* 读文件判断是否被禁用：Hidden=true 或 X-GNOME-Autostart-enabled=false */
static gboolean file_disabled(const char *path) {
  char *data = NULL;
  if (!g_file_get_contents(path, &data, NULL, NULL)) return TRUE;
  gboolean hidden = FALSE;
  char **lines = g_strsplit(data, "\n", -1);
  for (char **l = lines; *l; l++) {
    char *s = g_strstrip(*l);
    if (g_ascii_strncasecmp(s, "Hidden=", 7) == 0 && g_ascii_strcasecmp(s + 7, "true") == 0)
      hidden = TRUE;
    if (g_ascii_strncasecmp(s, "X-GNOME-Autostart-enabled=", 26) == 0 &&
        g_ascii_strcasecmp(s + 26, "false") == 0)
      hidden = TRUE;
  }
  g_strfreev(lines);
  g_free(data);
  return hidden;
}

gboolean uq_autostart_system_entry(void) {
  return g_file_test(system_path(), G_FILE_TEST_EXISTS);
}

gboolean uq_autostart_enabled(void) {
  if (g_file_test(user_path(), G_FILE_TEST_EXISTS))
    return !file_disabled(user_path());
  if (uq_autostart_system_entry())
    return !file_disabled(system_path());
  return FALSE;
}

static gboolean write_entry(gboolean hidden, GError **err) {
  char *dir = g_path_get_dirname(user_path());
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_free(dir);
    g_set_error_literal(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, "创建 autostart 目录失败");
    return FALSE;
  }
  g_free(dir);

  GString *s = g_string_new("[Desktop Entry]\n");
  g_string_append(s, "Type=Application\n");
  g_string_append(s, "Name=UQuick Daemon\n");
  g_string_append(s, "Comment=保持 UQuick 常驻，热键唤出零延迟\n");
  g_string_append_printf(s, "Exec=%s --daemon\n", uq_autostart_exec());
  g_string_append(s, "Terminal=false\n");
  g_string_append(s, "NoDisplay=true\n");
  if (hidden) g_string_append(s, "Hidden=true\n");
  g_string_append(s, "X-GNOME-Autostart-enabled=true\n");

  gboolean ok = g_file_set_contents(user_path(), s->str, (gssize)s->len, err);
  g_string_free(s, TRUE);
  return ok;
}

gboolean uq_autostart_set(gboolean on, GError **err) {
  if (on) return write_entry(FALSE, err);

  /* 关：有系统级条目就写屏蔽文件，否则直接删用户级文件 */
  if (uq_autostart_system_entry()) return write_entry(TRUE, err);
  if (g_unlink(user_path()) != 0 && g_file_test(user_path(), G_FILE_TEST_EXISTS)) {
    g_set_error_literal(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, "删除 autostart 文件失败");
    return FALSE;
  }
  return TRUE;
}

const char *uq_autostart_user_path(void) { return user_path(); }
