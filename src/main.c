/* main.c — 入口：客户端（极快）/ 守护进程（常驻）
 *
 * 热键路径：GNOME 自定义快捷键 -> `uquick --toggle`
 *   -> connect(unix socket) -> write("toggle <us>") -> exit
 *   -> 常驻进程在既有窗口上 present，首帧延迟 1~2 ms 量级。
 */
#include "uquick.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

const char *uq_socket_path(void) {
  static char path[256];
  if (path[0]) return path;
  const char *dir = g_getenv("XDG_RUNTIME_DIR");
  if (dir && *dir)
    g_snprintf(path, sizeof(path), "%s/uquick.sock", dir);
  else
    g_snprintf(path, sizeof(path), "/tmp/uquick.%u.sock", (unsigned)getuid());
  return path;
}

/* ---------- 客户端 ---------- */

static int connect_daemon(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct sockaddr_un sa = { .sun_family = AF_UNIX };
  g_strlcpy(sa.sun_path, uq_socket_path(), sizeof(sa.sun_path));
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int uq_ipc_send(const char *msg) {
  int fd = connect_daemon();
  if (fd < 0) return -1;
  size_t len = strlen(msg);
  ssize_t w = write(fd, msg, len);
  char reply[16];
  if (w == (ssize_t)len) {
    ssize_t r = read(fd, reply, sizeof(reply));
    (void)r;
  }
  close(fd);
  return 0;
}

/* 读回守护进程的应答（用于 --stats 之类的状态查询） */
static int ipc_query(const char *msg, char *out, size_t n) {
  int fd = connect_daemon();
  if (fd < 0) return -1;
  if (write(fd, msg, strlen(msg)) < 0) { close(fd); return -1; }
  ssize_t r = read(fd, out, n - 1);
  close(fd);
  if (r <= 0) return -1;
  out[r] = 0;
  g_strchomp(out);
  return 0;
}

/* 守护进程不在就拉起它；输出全部丢弃，绝不继承调用者的管道 */
static void spawn_daemon(void) {
  char exe[512];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) return;
  exe[n] = 0;
  char *argv[] = { exe, "--daemon", NULL };
  g_spawn_async(NULL, argv, NULL,
                G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                G_SPAWN_STDERR_TO_DEV_NULL,
                NULL, NULL, NULL, NULL);
}

static int client_raw(const char *msg, gboolean autostart) {
  if (uq_ipc_send(msg) == 0) return 0;
  if (!autostart) {
    g_printerr("uquick: 守护进程未运行（先执行 uquick --daemon 或 uquick --toggle）\n");
    return 1;
  }
  spawn_daemon();
  /* 等守护进程就绪：索引扫描 + 窗口创建，实测 60~120 ms */
  for (int i = 0; i < 200; i++) {
    g_usleep(5 * 1000);
    if (uq_ipc_send(msg) == 0) return 0;
  }
  g_printerr("uquick: 启动守护进程超时\n");
  return 1;
}

static int client_command(const char *cmd, gboolean autostart) {
  char msg[128];
  g_snprintf(msg, sizeof(msg), "%s %" G_GUINT64_FORMAT, cmd, (guint64)g_get_real_time());
  return client_raw(msg, autostart);
}

/* ---------- 守护进程 ---------- */

static int      srv_fd = -1;
static gboolean reloaded;

static gboolean on_socket(gint fd, GIOCondition cond, gpointer data) {
  (void)cond; (void)data;
  int c = accept(fd, NULL, NULL);
  if (c < 0) return G_SOURCE_CONTINUE;

  char buf[256];
  ssize_t n = read(c, buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = 0;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    char *sp = strchr(buf, ' ');
    char *cmd = buf;
    char *arg = NULL;
    if (sp) { *sp = 0; arg = sp + 1; }
    guint64 ts = arg ? g_ascii_strtoull(arg, NULL, 10) : 0;

    const char *reply = "ok";
    if (!cmd) {
      /* 忽略空消息 */
    } else if (strcmp(cmd, "status") == 0) {
      reply = uq_ui_is_visible() ? "visible" : "hidden";
    } else if (strcmp(cmd, "show") == 0) {
      uq_ui_set_pending(ts);
      uq_ui_show();
    } else if (strcmp(cmd, "toggle") == 0) {
      if (!uq_ui_is_visible()) uq_ui_set_pending(ts);
      uq_ui_toggle();
    } else if (strcmp(cmd, "hide") == 0) {
      uq_ui_hide();
    } else if (strcmp(cmd, "ping") == 0) {
      /* pong */
    } else if (strcmp(cmd, "query") == 0) {
      uq_ui_show();
      uq_ui_set_query(arg ? arg : "");
    } else if (strcmp(cmd, "run") == 0 && arg && *arg) {
      UqResult r[4];
      int n = uq_search(arg, r, 4, NULL, 0);
      if (n > 0) uq_launch(r[0].item, NULL);
      else g_printerr("uquick: 没有匹配 %s\n", arg);
    } else if (strcmp(cmd, "reload") == 0) {
      uq_index_load();
      reloaded = TRUE;
      if (uq_ui_is_visible()) uq_ui_show();
    } else if (strcmp(cmd, "quit") == 0) {
      uq_frec_flush();
      char buf2[8];
      int n2 = g_snprintf(buf2, sizeof(buf2), "%s\n", reply);
      if (write(c, buf2, (size_t)n2) < 0) { /* 忽略 */ }
      close(c);
      exit(0);
    }
    char buf2[16];
    int n2 = g_snprintf(buf2, sizeof(buf2), "%s\n", reply);
    if (write(c, buf2, (size_t)n2) < 0) { /* 对端已关闭 */ }
  }
  close(c);
  return G_SOURCE_CONTINUE;
}

static int bind_socket(void) {
  const char *path = uq_socket_path();
  unlink(path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct sockaddr_un sa = { .sun_family = AF_UNIX };
  g_strlcpy(sa.sun_path, path, sizeof(sa.sun_path));
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    g_printerr("uquick: bind %s 失败: %s\n", path, g_strerror(errno));
    close(fd);
    return -1;
  }
  chmod(path, 0600);
  if (listen(fd, 8) != 0) { close(fd); return -1; }
  return fd;
}

static void on_signal(int sig) {
  (void)sig;
  uq_frec_flush();
  _exit(0);
}

static void on_hup(int sig) {
  (void)sig;
  reloaded = TRUE;
}

static gboolean save_timer(gpointer data) {
  (void)data;
  if (reloaded) {
    reloaded = FALSE;
    uq_index_load();
  }
  uq_frec_flush();
  return G_SOURCE_CONTINUE;
}

static int run_daemon(void) {
  /* 单实例：已有守护进程就退出 */
  if (uq_ipc_send("ping 0") == 0) return 0;

  uq_frec_load();
  uq_index_load();

  if (srv_fd < 0) {
    srv_fd = bind_socket();
    if (srv_fd < 0) return 1;
    g_unix_fd_add(srv_fd, G_IO_IN, on_socket, NULL);
  }

  gtk_init();
  uq_ui_init();
  uq_ui_warmup();

  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  signal(SIGHUP, on_hup);
  g_timeout_add_seconds(3, save_timer, NULL);

  if (g_getenv("UQUICK_TRACE"))
    g_print("uquick: 索引 %u 项，扫描 %.2f ms，socket %s\n",
            uq_index_count(), (double)uq_index_scan_us() / 1000.0, uq_socket_path());

  uq_ui_run();
  return 0;
}

/* ---------- 基准与诊断 ---------- */

static int run_bench(void) {
  uq_frec_load();
  uq_index_load();
  printf("索引：%u 项，扫描耗时 %.2f ms（%s）\n",
         uq_index_count(), (double)uq_index_scan_us() / 1000.0,
         g_getenv("SNAP_NAME") ? "snap" : "dev");

  static const char *queries[] = {
    "chr", "term", "set", "wifi", "code", "fire", "文件", "设置",
    "1+2*3", "sqrt(2)", "sl", "gnome", "sh", "ty", "st", "ca", "ed", "nau", "scr",
  };
  const int nq = (int)G_N_ELEMENTS(queries);
  const int iterations = 20000;
  UqResult out[UQ_MAX_ROWS];

  /* 预热 */
  for (int i = 0; i < nq; i++) uq_search(queries[i], out, UQ_MAX_ROWS, NULL, 0);

  gint64 t0 = g_get_monotonic_time();
  guint hits_sum = 0;
  for (int it = 0; it < iterations; it++)
    hits_sum += (guint)uq_search(queries[it % nq], out, UQ_MAX_ROWS, NULL, 0);
  gint64 dt = g_get_monotonic_time() - t0;

  printf("搜索：%d 次 / %.1f ms，平均 %.2f µs/次（%.1f 万次/秒），命中均值 %.1f\n",
         iterations, (double)dt / 1000.0, (double)dt / iterations,
         (double)iterations * 1e6 / (double)dt / 1e4, (double)hits_sum / iterations);
  return 0;
}

static int run_explain(const char *q) {
  uq_frec_load();
  uq_index_load();
  UqResult r[20];
  int n = uq_search(q, r, 20, NULL, 0);
  printf("查询 %-12s -> %d 条\n", q, n);
  for (int i = 0; i < n; i++)
    printf("  %7.1f  kind=%d  %-28s | %s\n", r[i].score, r[i].item->kind,
           r[i].item->name, r[i].item->match);
  return 0;
}

/* 回归自检：计算器 + 匹配打分 + 索引/搜索；
 * 不依赖具体装了哪些应用，可在任何 Ubuntu 上跑 */
static int run_selftest(void) {
  int fail = 0;
  static const struct { const char *e; double want; } pos[] = {
    { "1+1", 2.0 }, { "(1+2)*3", 9.0 }, { "2^10", 1024.0 }, { "2^3^2", 512.0 },
    { "sqrt(16)", 4.0 }, { "-3*2", -6.0 }, { "10%3", 1.0 }, { "1_000+1", 1001.0 },
    { "floor(2.9)", 2.0 }, { "1/3", 1.0 / 3.0 }, { "2*pi/2", M_PI }, { "abs(-7)", 7.0 },
    { "1e3+1", 1001.0 }, { " 4 * ( 2 + 1 ) ", 12.0 },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(pos); i++) {
    double got = 0;
    if (!uq_calc_eval(pos[i].e, &got) || fabs(got - pos[i].want) > 1e-9) {
      g_printerr("FAIL calc  %-18s -> %s (%.17g, 期望 %.17g)\n", pos[i].e,
                 uq_calc_eval(pos[i].e, &got) ? "ok" : "err", got, pos[i].want);
      fail++;
    }
  }
  static const char *neg[] = { "1/0", "1+", "abc(1)", "", "5%0", "(1" };
  for (gsize i = 0; i < G_N_ELEMENTS(neg); i++) {
    double got = 0;
    if (uq_calc_eval(neg[i], &got)) {
      g_printerr("FAIL calc  %-18s 不该求值成功（得到 %.17g）\n", neg[i], got);
      fail++;
    }
  }

  /* 打分：前缀 > 词边界 > 子序列；且子序列不能靠关键词段"凑"出来 */
  double a = uq_fuzzy("firefox web", "fire");
  double b = uq_fuzzy("solidfire", "fire");
  double c = uq_fuzzy("firmware updater", "fire");
  if (!(a > b && b > c && c > 0)) {
    g_printerr("FAIL fuzzy fire: prefix=%.1f boundary=%.1f subseq=%.1f\n", a, b, c);
    fail++;
  }

  uq_frec_load();
  uq_index_load();
  if (uq_index_count() < 10) {
    g_printerr("FAIL index: 只索引到 %u 项\n", uq_index_count());
    fail++;
  }
  UqResult r[4];
  int n = uq_search("set", r, 4, NULL, 0);
  if (n <= 0) { g_printerr("FAIL search: \"set\" 无命中\n"); fail++; }
  if (uq_search("zzzzqqqq", r, 4, NULL, 0) != 0) {
    g_printerr("FAIL search: 乱串不该有命中\n"); fail++;
  }

  if (fail) {
    printf("自检失败：%d 项\n", fail);
    return 1;
  }
  printf("自检通过：计算器 %zu 正例 + %zu 反例，打分/索引/搜索正常（索引 %u 项，扫描 %.2f ms）\n",
         G_N_ELEMENTS(pos), G_N_ELEMENTS(neg), uq_index_count(),
         (double)uq_index_scan_us() / 1000.0);
  return 0;
}

static int run_stats(void) {
  uq_frec_load();
  uq_index_load();
  int fd = connect_daemon();
  printf("索引项      : %u\n", uq_index_count());
  printf("扫描耗时    : %.2f ms\n", (double)uq_index_scan_us() / 1000.0);
  printf("socket      : %s\n", uq_socket_path());
  printf("守护进程    : %s\n", fd >= 0 ? "运行中" : "未运行");
  if (fd >= 0) {
    close(fd);
    char st[16];
    if (ipc_query("status", st, sizeof(st)) == 0)
      printf("面板        : %s\n", strcmp(st, "visible") == 0 ? "显示中" : "隐藏");
  }
  printf("配置目录    : %s\n", g_get_user_config_dir());
  printf("状态目录    : %s\n", g_get_user_state_dir());
  printf("数据目录    : %s\n", g_get_user_data_dir());
  return 0;
}

static void usage(void) {
  printf("uquick %s — 极速启动器\n\n", UQ_VERSION);
  printf("用法：\n");
  printf("  uquick                 切换面板（未运行则自动拉起守护进程）\n");
  printf("  uquick --toggle        同上\n");
  printf("  uquick --show|--hide   显示 / 隐藏\n");
  printf("  uquick --query TEXT    显示并预置查询\n");
  printf("  uquick --run TEXT      按查询结果直接启动首个命中（脚本用）\n");
  printf("  uquick --daemon        前台运行守护进程\n");
  printf("  uquick --reload        重载索引与自定义命令\n");
  printf("  uquick --quit          退出守护进程\n");
  printf("  uquick --bench         索引 + 搜索基准\n");
  printf("  uquick --bench-show    热键到首帧延迟基准（需要显示环境）\n");
  printf("  uquick --stats         运行状态\n");
  printf("  uquick --selftest      计算器/打分/索引回归自检\n");
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "");

  if (argc > 1) {
    const char *a = argv[1];
    if (!strcmp(a, "--daemon"))                     return run_daemon();
    if (!strcmp(a, "--toggle") || !strcmp(a, "-t"))  return client_command("toggle", TRUE);
    if (!strcmp(a, "--show") || !strcmp(a, "-s"))    return client_command("show", TRUE);
    if (!strcmp(a, "--hide"))                        return client_command("hide", FALSE);
    if (!strcmp(a, "--run") && argc > 2) {
      char *msg = g_strconcat("run ", argv[2], NULL);
      int rc = client_raw(msg, TRUE);
      g_free(msg);
      return rc;
    }
    if (!strcmp(a, "--query") && argc > 2) {
      char *msg = g_strconcat("query ", argv[2], NULL);
      int rc = client_raw(msg, TRUE);
      g_free(msg);
      return rc;
    }
    if (!strcmp(a, "--reload"))                      return client_command("reload", FALSE);
    if (!strcmp(a, "--quit"))                        return client_command("quit", FALSE);
    if (!strcmp(a, "--bench"))                       return run_bench();
    if (!strcmp(a, "--explain") && argc > 2)         return run_explain(argv[2]);
    if (!strcmp(a, "--selftest"))                    return run_selftest();
    if (!strcmp(a, "--stats"))                       return run_stats();
    if (!strcmp(a, "--version") || !strcmp(a, "-v")) { printf("uquick %s\n", UQ_VERSION); return 0; }
    if (!strcmp(a, "--help") || !strcmp(a, "-h"))    { usage(); return 0; }
    if (!strcmp(a, "--bench-show")) {
      uq_frec_load();
      uq_index_load();
      gtk_init();
      uq_ui_init();
      uq_ui_bench();
      uq_ui_run();
      return 0;
    }
    g_printerr("uquick: 未知参数 %s（--help 查看用法）\n", a);
    return 2;
  }
  return client_command("toggle", TRUE);
}
