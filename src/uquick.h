/* uquick.h — 共享类型与接口
 *
 * 设计约束（性能优先）：
 *  - 常驻进程持有窗口与索引：热键 -> socket -> present，量级 1 ms；
 *  - 客户端路径不碰 GTK/GLib 之外的重活，只做 connect+write+exit；
 *  - 搜索全程无分配（haystack 预折叠），单次查询 O(n) 且 n 约 2k。
 */
#pragma once

#include <glib.h>
#include <gtk/gtk.h>

#define UQ_APP_ID    "io.github.suveng.uquick"
#define UQ_VERSION   "0.1.3"
#define UQ_MAX_ROWS  10
#define UQ_MAX_TOKENS 6

typedef enum {
  UQ_KIND_APP = 0,   /* .desktop 应用 */
  UQ_KIND_ACTION,    /* 内置系统动作 */
  UQ_KIND_COMMAND,   /* ~/.config/uquick/commands.tsv */
  UQ_KIND_SHELL,     /* "> cmd" 交给 /bin/sh */
  UQ_KIND_CALC,      /* 计算结果 */
  UQ_KIND_URI,       /* 网址 */
  UQ_KIND_PATH,      /* 文件路径 */
} UqKind;

typedef struct {
  char    *name;      /* 展示名（UTF-8） */
  char    *sub;       /* 右侧/副标题（UTF-8，可 NULL） */
  char    *key;       /* frecency 键（可 NULL） */
  char    *match;     /* 已小写折叠的匹配串：name + 关键词 */
  guint16  name_len;  /* match 中「名称」部分的字节数（子序列只匹配这一段） */
  char    *path;      /* APP: .desktop 路径 */
  char    *arg;       /* APP: 执行回退串 / ACTION,COMMAND,SHELL: 命令 / URI,PATH: 目标 */
  char    *icon;      /* 图标名或绝对路径 */
  UqKind   kind;
  gboolean terminal;
  gboolean transient;
  double   boost;     /* 使用热度（已含时间衰减） */
} UqItem;

typedef struct {
  UqItem *items;
  guint   len;
  guint   cap;
} UqIndex;

typedef struct {
  UqItem *item;
  double  score;
} UqResult;

/* ---- core.c: 索引、模糊匹配、热度 ---- */
void    uq_index_load(void);          /* 扫描（可用工作线程调用后回主线程） */
void    uq_index_reload(void);
void    uq_index_free(void);
guint   uq_index_count(void);
guint64 uq_index_scan_us(void);

double  uq_fuzzy(const char *hay, const char *token);
int     uq_search(const char *query, UqResult *out, int max,
                  UqItem *extra, int n_extra);

void    uq_frec_load(void);
void    uq_frec_bump(const char *key);   /* 主线程，置脏 */
void    uq_frec_flush(void);             /* 写盘（去抖后调用） */

/* ---- pinyin.c ---- */
/* 把 UTF-8 文本转成（首字母串, 全拼串）；返回是否含汉字。
 * 非汉字按小写原样并入，分隔符归一成空格。 */
gboolean uq_pinyin(const char *utf8, GString *initials, GString *full);

/* ---- calc.c ---- */
gboolean uq_calc_eval(const char *expr, double *out);

/* ---- launch.c ---- */
void    uq_launch(UqItem *it, GtkWindow *win);
void    uq_copy_text(const char *text);
void    uq_load_commands(void);          /* 读 commands.tsv 并并入索引 */

/* ---- autostart.c ---- */
gboolean    uq_autostart_enabled(void);
gboolean    uq_autostart_set(gboolean on, GError **err);
gboolean    uq_autostart_system_entry(void);
const char *uq_autostart_exec(void);
const char *uq_autostart_user_path(void);

/* ---- settings.c / tray.c ---- */
void        uq_settings_show(void);
gboolean    uq_settings_is_open(void);
gboolean    uq_tray_init(GError **err);   /* 顶栏托盘（SNI + dbusmenu） */
gboolean    uq_tray_active(void);
void        uq_tray_update(void);         /* 自启状态变化后刷新菜单勾选 */
gboolean    uq_tray_write_icon_png(const char *path, int size);
void        uq_quit(void);                /* 存热度并退出守护进程 */

/* ---- ui.c ---- */
void    uq_ui_init(void);
void    uq_ui_show(void);
void    uq_ui_hide(void);
void    uq_ui_toggle(void);
gboolean uq_ui_is_visible(void);
void    uq_ui_run(void);
void    uq_ui_set_pending(guint64 unix_us);   /* 延迟测量用 */
void    uq_ui_set_query(const char *text);     /* 预置查询（脚本/截图） */
void    uq_ui_refresh(void);                   /* 索引重建后按当前查询重渲染 */
void    uq_ui_warmup(void);                    /* 启动即预热首帧，避免首次热键慢 190 ms */
void    uq_ui_bench(void);                    /* 打开 -> 首帧，打印耗时 */

/* ---- main.c: IPC ---- */
int     uq_ipc_send(const char *msg);         /* 0 成功，-1 无守护进程 */
const char *uq_socket_path(void);
