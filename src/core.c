/* core.c — .desktop 索引、模糊匹配、使用热度
 *
 * 索引在启动时扫一遍 XDG applications 目录并驻留内存；
 * 搜索只做整数/浮点打分，不做正则、不做额外分配。
 */
#define _GNU_SOURCE
#include "uquick.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>

static UqIndex  idx;
static guint64  scan_us;

/* ---------- 小工具 ---------- */

static inline char fold_c(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* 去首尾空白、内部连续空白压成一个空格，并折叠大小写 */
static char *normalize_query(const char *s) {
  GString *g = g_string_sized_new(strlen(s) + 1);
  gboolean space = TRUE;
  for (const char *p = s; *p; p++) {
    char c = *p;
    if (c == ' ' || c == '\t' || c == '\n') {
      if (!space) { g_string_append_c(g, ' '); space = TRUE; }
    } else {
      g_string_append_c(g, fold_c(c));
      space = FALSE;
    }
  }
  while (g->len && g->str[g->len - 1] == ' ') g_string_truncate(g, g->len - 1);
  return g_string_free(g, FALSE);
}

/* ---------- 热度（frecency） ---------- */

typedef struct { guint32 count; gint64 last; double boost; } Frec;

static GHashTable *frec;
static gboolean    frec_dirty;
static char       *frec_path;

static double frec_compute(guint32 count, gint64 last) {
  if (!count) return 0.0;
  double days = (double)(g_get_real_time() - last) / (86400.0 * 1e6);
  if (days < 0) days = 0;
  return 45.0 * log2(1.0 + (double)count) * exp(-days / 45.0);
}

static const char *frec_dir(void) {
  static char *dir;
  if (!dir) dir = g_build_filename(g_get_user_state_dir(), "uquick", NULL);
  return dir;
}

void uq_frec_load(void) {
  if (!frec) frec = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  frec_path = g_build_filename(frec_dir(), "frecency", NULL);
  char *data = NULL;
  if (g_file_get_contents(frec_path, &data, NULL, NULL)) {
    char **lines = g_strsplit(data, "\n", -1);
    for (char **l = lines; *l; l++) {
      char *tab1 = strchr(*l, '\t');
      if (!tab1) continue;
      *tab1 = 0;
      char *tab2 = strchr(tab1 + 1, '\t');
      if (!tab2) continue;
      *tab2 = 0;
      guint32 count = (guint32)g_ascii_strtoull(tab1 + 1, NULL, 10);
      gint64 last = (gint64)g_ascii_strtoll(tab2 + 1, NULL, 10);
      Frec *f = g_new0(Frec, 1);
      f->count = count;
      f->last = last;
      f->boost = frec_compute(count, last);
      g_hash_table_replace(frec, g_strdup(*l), f);
    }
    g_strfreev(lines);
    g_free(data);
  }
}

static const Frec *frec_get(const char *key) {
  if (!key || !frec) return NULL;
  return g_hash_table_lookup(frec, key);
}

void uq_frec_bump(const char *key) {
  if (!key || !*key) return;
  if (!frec) uq_frec_load();
  Frec *f = g_hash_table_lookup(frec, key);
  if (!f) {
    f = g_new0(Frec, 1);
    g_hash_table_replace(frec, g_strdup(key), f);
  }
  f->count++;
  f->last = g_get_real_time();
  f->boost = frec_compute(f->count, f->last);
  /* 同步到驻留项，避免下次搜索前还看到旧值 */
  for (guint i = 0; i < idx.len; i++)
    if (idx.items[i].key && strcmp(idx.items[i].key, key) == 0)
      idx.items[i].boost = f->boost;
  frec_dirty = TRUE;
}

void uq_frec_flush(void) {
  if (!frec_dirty || !frec_path) return;
  GString *out = g_string_new(NULL);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, frec);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    Frec *f = v;
    g_string_append_printf(out, "%s\t%u\t%" G_GINT64_FORMAT "\n", (char *)k, f->count, f->last);
  }
  char *dir = g_path_get_dirname(frec_path);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);
  char *tmp = g_strconcat(frec_path, ".tmp", NULL);
  if (g_file_set_contents(tmp, out->str, (gssize)out->len, NULL))
    g_rename(tmp, frec_path);
  g_free(tmp);
  g_string_free(out, TRUE);
  frec_dirty = FALSE;
}

/* ---------- 匹配打分 ---------- */

/* 只在前 hlimit 字节里做「连续子串」与「子序列」匹配；
 * 子序列限定在名称段，避免关键词里的零散字母把无关应用顶上来 */
static double fuzzy_core(const char *hay, size_t hlimit, const char *tok) {
  size_t hl = strlen(hay), tl = strlen(tok);
  if (tl == 0) return 0;
  if (tl > hl) return 0;
  if (hlimit > hl || hlimit == 0) hlimit = hl;

  /* 连续子串允许命中关键词段；子序列只限名称段 */
  const char *p = memmem(hay, hl, tok, tl);
  if (p) {
    size_t pos = (size_t)(p - hay);
    double base;
    if (pos == 0) base = 1000.0;
    else if (strchr(" -_./:(", hay[pos - 1])) base = 700.0;
    else { base = 400.0 - (double)pos; if (base < 60.0) base = 60.0; }
    double dens = 1.0 - (double)hl / 400.0;
    if (dens < 0) dens = 0;
    return base + 160.0 * dens + 4.0 * (double)tl;
  }

  /* 子序列（可跨词），只给低分，避免噪音 */
  size_t i = 0, runs = 0, first = 0;
  gboolean started = FALSE;
  for (size_t j = 0; j < hlimit && i < tl; j++) {
    if (hay[j] == tok[i]) {
      if (!started) { started = TRUE; first = j; }
      i++;
      if (i < tl && j + 1 < hl && hay[j + 1] == tok[i]) runs++;
    }
  }
  if (i < tl) return 0;
  double base = 140.0 - (double)first;
  if (base < 10.0) base = 10.0;
  return base + 12.0 * (double)runs;
}

double uq_fuzzy(const char *hay, const char *tok) {
  return fuzzy_core(hay, strlen(hay), tok);
}

/* ---------- 索引扫描 ---------- */

static GHashTable *seen_ids;

static gboolean split_contains(GKeyFile *kf, const char *key, const char *want) {
  gsize n = 0;
  char **list = g_key_file_get_string_list(kf, "Desktop Entry", key, &n, NULL);
  gboolean found = FALSE;
  for (gsize i = 0; i < n && !found; i++)
    if (g_strcmp0(list[i], want) == 0) found = TRUE;
  g_strfreev(list);
  return found;
}

/* 把名称的拼音（首字母 + 全拼）追加到匹配串尾部：
 * 连续匹配能命中拼音段，子序列仍被 name_len 限制在名称段内，避免噪音。 */
static void append_pinyin(GString *hay, const char *text) {
  GString *ini = g_string_sized_new(16), *full = g_string_sized_new(64);
  if (uq_pinyin(text, ini, full) && ini->len > 0) {
    g_string_append_c(hay, ' ');
    g_string_append(hay, ini->str);
    g_string_append_c(hay, ' ');
    g_string_append(hay, full->str);
  }
  g_string_free(ini, TRUE);
  g_string_free(full, TRUE);
}

static void add_item(UqItem it) {
  if (idx.len == idx.cap) {
    idx.cap = idx.cap ? idx.cap * 2 : 512;
    idx.items = g_realloc(idx.items, sizeof(UqItem) * idx.cap);
  }
  idx.items[idx.len++] = it;
}

static void parse_desktop(const char *path) {
  GKeyFile *kf = g_key_file_new();
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    g_key_file_free(kf);
    return;
  }
  if (!g_key_file_has_group(kf, "Desktop Entry")) goto out;
  char *type = g_key_file_get_string(kf, "Desktop Entry", "Type", NULL);
  if (g_strcmp0(type, "Application") != 0) { g_free(type); goto out; }
  g_free(type);

  if (g_key_file_get_boolean(kf, "Desktop Entry", "NoDisplay", NULL)) goto out;
  if (g_key_file_get_boolean(kf, "Desktop Entry", "Hidden", NULL)) goto out;
  if (g_key_file_has_key(kf, "Desktop Entry", "OnlyShowIn", NULL) &&
      !split_contains(kf, "OnlyShowIn", "GNOME") &&
      !split_contains(kf, "OnlyShowIn", "Unity")) goto out;
  if (split_contains(kf, "NotShowIn", "GNOME")) goto out;

  char *id = g_path_get_basename(path);
  size_t idlen = strlen(id);
  if (idlen > 8) id[idlen - 8] = 0; /* 去掉 ".desktop" */
  if (g_hash_table_contains(seen_ids, id)) { g_free(id); goto out; }

  char *exec = g_key_file_get_string(kf, "Desktop Entry", "Exec", NULL);
  if (!exec || !*exec) { g_free(exec); g_free(id); goto out; }

  char *tryexec = g_key_file_get_string(kf, "Desktop Entry", "TryExec", NULL);
  if (tryexec && *tryexec && !g_find_program_in_path(tryexec)) {
    g_free(tryexec); g_free(exec); g_free(id); goto out;
  }
  g_free(tryexec);

  char *name = g_key_file_get_locale_string(kf, "Desktop Entry", "Name", NULL, NULL);
  if (!name || !*name) { g_free(name); g_free(exec); g_free(id); goto out; }
  char *generic = g_key_file_get_locale_string(kf, "Desktop Entry", "GenericName", NULL, NULL);
  char *kws = g_key_file_get_locale_string(kf, "Desktop Entry", "Keywords", NULL, NULL);
  char *cats = g_key_file_get_string(kf, "Desktop Entry", "Categories", NULL);
  char *icon = g_key_file_get_string(kf, "Desktop Entry", "Icon", NULL);

  GString *hay = g_string_sized_new(strlen(name) + (kws ? strlen(kws) : 0) + 32);
  for (const char *p = name; *p; p++) g_string_append_c(hay, fold_c(*p));
  if (generic && *generic) {
    g_string_append_c(hay, ' ');
    for (const char *p = generic; *p; p++) g_string_append_c(hay, fold_c(*p));
  }
  if (kws && *kws) {
    g_string_append_c(hay, ' ');
    for (const char *p = kws; *p; p++) g_string_append_c(hay, (*p == ';') ? ' ' : fold_c(*p));
  }
  if (cats && *cats) {
    g_string_append_c(hay, ' ');
    for (const char *p = cats; *p; p++) g_string_append_c(hay, (*p == ';') ? ' ' : fold_c(*p));
  }
  if (id) {
    g_string_append_c(hay, ' ');
    for (const char *p = id; *p; p++) g_string_append_c(hay, fold_c(*p));
  }
  append_pinyin(hay, name);

  UqItem it = {0};
  it.kind = UQ_KIND_APP;
  it.name = name;
  it.sub = generic && *generic ? generic : NULL;
  if (!it.sub) g_free(generic);
  it.match = g_string_free(hay, FALSE);
  it.name_len = (guint16)strlen(name);
  it.path = g_strdup(path);
  it.arg = exec;
  it.icon = icon;
  it.key = id;
  it.terminal = g_key_file_get_boolean(kf, "Desktop Entry", "Terminal", NULL);
  {
    const Frec *f = frec_get(it.key);
    it.boost = f ? f->boost : 0.0;
  }
  g_hash_table_add(seen_ids, g_strdup(it.key));
  add_item(it);
  g_free(kws);
  g_free(cats);
out:
  g_key_file_free(kf);
}

static void scan_dir(const char *dir, int depth) {
  GDir *d = g_dir_open(dir, 0, NULL);
  if (!d) return;
  const char *n;
  /* 先收集再递归，避免遍历中打开目录带来的抖动 */
  while ((n = g_dir_read_name(d))) {
    if (n[0] == '.') continue;
    char *full = g_build_filename(dir, n, NULL);
    if (g_str_has_suffix(n, ".desktop")) {
      parse_desktop(full);
    } else if (depth < 2) {
      if (g_file_test(full, G_FILE_TEST_IS_DIR)) scan_dir(full, depth + 1);
    }
    g_free(full);
  }
  g_dir_close(d);
}

static int item_cmp_name(gconstpointer a, gconstpointer b) {
  return g_strcmp0(((const UqItem *)a)->name, ((const UqItem *)b)->name);
}

/* 内置系统动作 */
static const struct { const char *key, *name, *sub, *icon, *cmd; } actions[] = {
  { "act:terminal", "终端",        "打开终端",          "utilities-terminal",              "x-terminal-emulator" },
  { "act:files",    "文件",        "主目录",            "system-file-manager",             "xdg-open \"$HOME\"" },
  { "act:settings", "设置",        "系统设置",          "preferences-system",              "gnome-control-center" },
  { "act:wifi",     "Wi-Fi 设置",  "网络与无线",        "network-wireless",                "gnome-control-center wifi" },
  { "act:bt",       "蓝牙设置",    "设备连接",          "bluetooth-active",                "gnome-control-center bluetooth" },
  { "act:display",  "显示设置",    "分辨率与缩放",      "video-display",                   "gnome-control-center display" },
  { "act:sound",    "声音设置",    "音量与输出",        "audio-volume-high",               "gnome-control-center sound" },
  { "act:power",    "电源设置",    "省电与电池",        "battery-good",                    "gnome-control-center power" },
  { "act:lock",     "锁屏",        "立即锁定会话",      "system-lock-screen",              "loginctl lock-session" },
  { "act:suspend",  "挂起",        "睡眠",              "system-suspend",                  "systemctl suspend" },
  { "act:logout",   "注销",        "退出当前会话",      "system-log-out",                  "gnome-session-quit --logout --no-prompt" },
  { "act:reboot",   "重启",        "重新启动计算机",    "system-reboot",                   "gnome-session-quit --reboot --no-prompt" },
  { "act:poweroff", "关机",        "关闭计算机",        "system-shutdown",                 "gnome-session-quit --power-off --no-prompt" },
};

static void add_actions(void) {
  for (gsize i = 0; i < G_N_ELEMENTS(actions); i++) {
    UqItem it = {0};
    it.kind = UQ_KIND_ACTION;
    it.name = g_strdup(actions[i].name);
    it.sub = g_strdup(actions[i].sub);
    it.icon = g_strdup(actions[i].icon);
    it.arg = g_strdup(actions[i].cmd);
    it.key = g_strdup(actions[i].key);
    GString *hay = g_string_new(NULL);
    g_string_append_printf(hay, "%s %s", actions[i].key + 4, actions[i].name);
    for (char *p = hay->str; *p; p++) *p = fold_c(*p);
    append_pinyin(hay, actions[i].name);
    it.match = g_string_free(hay, FALSE);
    it.name_len = (guint16)strlen(it.match);
    const Frec *f = frec_get(it.key);
    it.boost = f ? f->boost : 0.0;
    add_item(it);
  }

  /* 开机自启开关（名称固定、状态放副标题，便于稳定检索） */
  {
    gboolean on = uq_autostart_enabled();
    GString *hay = g_string_new("开机自启 autostart boot startup login ");
    for (char *p = hay->str; *p; p++) *p = fold_c(*p);
    append_pinyin(hay, "开机自启");
    UqItem it = {0};
    it.kind = UQ_KIND_ACTION;
    it.name = g_strdup("开机自启");
    it.sub = g_strdup(on ? "已开启 · 回车关闭" : "已关闭 · 回车开启");
    it.icon = g_strdup("system-run");
    it.arg = g_strdup("act:autostart");
    it.key = g_strdup("act:autostart");
    it.match = g_string_free(hay, FALSE);
    it.name_len = (guint16)strlen(it.match);
    add_item(it);
  }
}

/* 用户自定义命令：~/.config/uquick/commands.tsv
 * 每行 name<TAB>command[<TAB>关键词]，# 开头为注释 */
void uq_load_commands(void) {
  char *path = g_build_filename(g_get_user_config_dir(), "uquick", "commands.tsv", NULL);
  char *data = NULL;
  if (g_file_get_contents(path, &data, NULL, NULL)) {
    char **lines = g_strsplit(data, "\n", -1);
    for (char **l = lines; *l; l++) {
      char *line = g_strstrip(*l);
      if (!*line || *line == '#') continue;
      char *tab1 = strchr(line, '\t');
      if (!tab1) continue;
      *tab1 = 0;
      char *cmd = tab1 + 1;
      char *tab2 = strchr(cmd, '\t');
      char *extra = NULL;
      if (tab2) { *tab2 = 0; extra = tab2 + 1; }
      char *name = g_strstrip(line);
      cmd = g_strstrip(cmd);
      if (!*name || !*cmd) continue;

      UqItem it = {0};
      it.kind = UQ_KIND_COMMAND;
      it.name = g_strdup(name);
      it.sub = g_strdup(cmd);
      it.arg = g_strdup(cmd);
      it.icon = g_strdup("utilities-terminal-symbolic");
      it.key = g_strdup_printf("cmd:%s", name);
      GString *hay = g_string_new(NULL);
      g_string_append_printf(hay, "%s %s %s", name, cmd, extra ? extra : "");
      for (char *p = hay->str; *p; p++) *p = fold_c(*p);
      append_pinyin(hay, name);
      it.match = g_string_free(hay, FALSE);
      it.name_len = (guint16)strlen(name);
      const Frec *f = frec_get(it.key);
      it.boost = f ? f->boost : 0.0;
      add_item(it);
    }
    g_strfreev(lines);
    g_free(data);
  }
  g_free(path);
}

void uq_index_free(void) {
  for (guint i = 0; i < idx.len; i++) {
    UqItem *it = &idx.items[i];
    g_free(it->name); g_free(it->sub); g_free(it->key); g_free(it->match);
    g_free(it->path); g_free(it->arg); g_free(it->icon);
  }
  g_clear_pointer(&idx.items, g_free);
  idx.len = idx.cap = 0;
  g_clear_pointer(&seen_ids, g_hash_table_destroy);
}

guint uq_index_count(void) { return idx.len; }
guint64 uq_index_scan_us(void) { return scan_us; }

void uq_index_load(void) {
  gint64 t0 = g_get_monotonic_time();
  uq_index_free();
  if (!frec) uq_frec_load();
  seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  /* 用户目录优先（覆盖系统同名条目） */
  char *user = g_build_filename(g_get_user_data_dir(), "applications", NULL);
  scan_dir(user, 0);
  g_free(user);

  const char *dirs = g_getenv("XDG_DATA_DIRS");
  if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
  char **list = g_strsplit(dirs, ":", -1);
  for (char **d = list; *d; d++) {
    if (!**d) continue;
    char *p = g_build_filename(*d, "applications", NULL);
    scan_dir(p, 0);
    g_free(p);
  }
  g_strfreev(list);

  add_actions();
  uq_load_commands();

  /* 名称排序，使空查询下的同分项稳定 */
  if (idx.len) qsort(idx.items, idx.len, sizeof(UqItem), item_cmp_name);
  scan_us = (guint64)(g_get_monotonic_time() - t0);
}

void uq_index_reload(void) { uq_index_load(); }

/* ---------- 搜索 ---------- */

/* 结果集：定长有序数组，插入只与榜尾比较 */
typedef struct {
  UqResult *v;
  int       n, cap;
} Top;

static inline void top_push(Top *t, UqItem *it, double score) {
  if (score <= 0) return;
  int i = t->n;
  if (i == t->cap && score <= t->v[t->cap - 1].score) return;
  if (i < t->cap) t->n++;
  else i = t->cap - 1;               /* 覆盖榜尾 */
  while (i > 0 && t->v[i - 1].score < score) {
    t->v[i] = t->v[i - 1];
    i--;
  }
  t->v[i].item = it;
  t->v[i].score = score;
}

static inline double score_item(const UqItem *it, const char *q,
                                const char *const *tokens, int ntokens) {
  double s = 0;
  if (ntokens == 0) {
    s = 100.0;
  } else {
    for (int i = 0; i < ntokens; i++) {
      double one = fuzzy_core(it->match, it->name_len, tokens[i]);
      if (one <= 0) return 0;
      s += one;
    }
    if (strcmp(it->match, q) == 0) s += 2000.0;
  }
  return s + it->boost;
}

int uq_search(const char *query, UqResult *out, int max,
              UqItem *extra, int n_extra) {
  if (max <= 0) return 0;
  char *q = normalize_query(query ? query : "");

  const char *tokens[UQ_MAX_TOKENS];
  int ntokens = 0;
  if (*q) {
    char *save = NULL;
    for (char *t = strtok_r(q, " ", &save); t && ntokens < UQ_MAX_TOKENS;
         t = strtok_r(NULL, " ", &save))
      tokens[ntokens++] = t;
  }

  Top top = { out, 0, max };

  /* 动态项（计算/网址/路径/shell）优先占位 */
  for (int i = 0; i < n_extra; i++)
    top_push(&top, &extra[i], score_item(&extra[i], q, tokens, ntokens) + 4000.0);

  for (guint i = 0; i < idx.len; i++) {
    double s = score_item(&idx.items[i], q, tokens, ntokens);
    if (s > 0) top_push(&top, &idx.items[i], s);
  }

  g_free(q);
  return top.n;
}
