/* tray.c — 顶栏托盘图标 + 菜单（StatusNotifierItem + com.canonical.dbusmenu）
 *
 * 为什么手写协议而不用 libayatana-appindicator：那是 GTK3 库，和本项目的 GTK4
 * 进程不能混用；SNI/dbusmenu 都是纯 D-Bus，用 GDBus 实现没有额外运行时依赖。
 *
 * 图标用 IconPixmap（cairo 现画 ARGB32）而不是 IconName：classic snap 的图标
 * 不在宿主机的图标主题里，按名字查会查不到（dev 安装时也只需要一份代码）。
 */
#include "uquick.h"

#include <cairo.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#define SNI_PATH  "/StatusNotifierItem"
#define MENU_PATH "/MenuBar"

#define MENU_OPEN     1
#define MENU_SETTINGS 2
#define MENU_SEP1     3
#define MENU_AUTOSTART 4
#define MENU_SEP2     5
#define MENU_QUIT     6

static GDBusConnection *bus;
static guint  sni_reg, menu_reg;
static char  *sni_name;
static guint32 revision = 1;

static const char sni_xml[] =
  "<node>"
  " <interface name='org.kde.StatusNotifierItem'>"
  "  <property name='Category' type='s' access='read'/>"
  "  <property name='Id' type='s' access='read'/>"
  "  <property name='Title' type='s' access='read'/>"
  "  <property name='Status' type='s' access='read'/>"
  "  <property name='IconName' type='s' access='read'/>"
  "  <property name='IconPixmap' type='a(iiay)' access='read'/>"
  "  <property name='Menu' type='o' access='read'/>"
  "  <property name='ItemIsMenu' type='b' access='read'/>"
  "  <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
  "  <method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "  <method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "  <method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "  <method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
  "  <signal name='NewIcon'/>"
  "  <signal name='NewStatus'><arg type='s'/></signal>"
  " </interface>"
  "</node>";

static const char menu_xml[] =
  "<node>"
  " <interface name='com.canonical.dbusmenu'>"
  "  <property name='Version' type='u' access='read'/>"
  "  <property name='TextDirection' type='s' access='read'/>"
  "  <property name='Status' type='s' access='read'/>"
  "  <property name='IconThemePath' type='as' access='read'/>"
  "  <method name='GetLayout'>"
  "   <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "   <arg type='as' direction='in'/>"
  "   <arg type='u' direction='out'/><arg type='(ia{sv}av)' direction='out'/>"
  "  </method>"
  "  <method name='GetGroupProperties'>"
  "   <arg type='ai' direction='in'/><arg type='as' direction='in'/>"
  "   <arg type='a(ia{sv})' direction='out'/>"
  "  </method>"
  "  <method name='GetProperty'>"
  "   <arg type='i' direction='in'/><arg type='s' direction='in'/>"
  "   <arg type='v' direction='out'/>"
  "  </method>"
  "  <method name='Event'>"
  "   <arg type='i' direction='in'/><arg type='s' direction='in'/>"
  "   <arg type='v' direction='in'/><arg type='u' direction='in'/>"
  "  </method>"
  "  <method name='EventGroup'><arg type='a(isvu)' direction='in'/>"
  "   <arg type='ai' direction='out'/></method>"
  "  <method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
  "  <method name='AboutToShowGroup'><arg type='ai' direction='in'/>"
  "   <arg type='ai' direction='out'/><arg type='ai' direction='out'/></method>"
  "  <signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
  "  <signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})'/><arg type='a(ias)'/></signal>"
  " </interface>"
  "</node>";

/* ---------- 图标：与 SVG 同款，cairo 现画 ----------
 * 128×128 坐标系，调用方自己 cairo_scale；<=24px 时走简化版（去掉高光/暗角/投影，
 * 加粗描边），否则小图标会糊成一团。 */

static void rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r) {
  double x2 = x + w, y2 = y + h;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x2 - r, y + r, r, -M_PI_2, 0);
  cairo_arc(cr, x2 - r, y2 - r, r, 0, M_PI_2);
  cairo_arc(cr, x + r, y2 - r, r, M_PI_2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
  cairo_close_path(cr);
}

static void bolt_path(cairo_t *cr) {
  cairo_move_to(cr, 76, 16);
  cairo_line_to(cr, 44, 69);
  cairo_line_to(cr, 62, 69);
  cairo_line_to(cr, 52, 112);
  cairo_line_to(cr, 88, 54);
  cairo_line_to(cr, 68, 54);
  cairo_close_path(cr);
}

static void draw_icon(cairo_t *cr, gboolean small) {
  /* 底色：左上 -> 右下三段渐变 */
  cairo_pattern_t *bg = cairo_pattern_create_linear(10, 4, 115, 124);
  cairo_pattern_add_color_stop_rgb(bg, 0.00, 0.357, 0.608, 1.000);   /* #5B9BFF */
  cairo_pattern_add_color_stop_rgb(bg, 0.48, 0.180, 0.416, 0.910);   /* #2E6AE8 */
  cairo_pattern_add_color_stop_rgb(bg, 1.00, 0.086, 0.208, 0.624);   /* #16359F */
  rounded_rect_path(cr, 4, 4, 120, 120, 31);
  cairo_set_source(cr, bg);
  cairo_fill(cr);
  cairo_pattern_destroy(bg);

  if (!small) {
    /* 顶部径向高光 */
    cairo_pattern_t *hl = cairo_pattern_create_radial(41, 2.4, 0, 41, 2.4, 102);
    cairo_pattern_add_color_stop_rgba(hl, 0.00, 1, 1, 1, 0.30);
    cairo_pattern_add_color_stop_rgba(hl, 0.55, 1, 1, 1, 0.05);
    cairo_pattern_add_color_stop_rgba(hl, 1.00, 1, 1, 1, 0.00);
    rounded_rect_path(cr, 4, 4, 120, 120, 31);
    cairo_set_source(cr, hl);
    cairo_fill(cr);
    cairo_pattern_destroy(hl);

    /* 底部压暗，增加体积感 */
    cairo_pattern_t *vig = cairo_pattern_create_linear(0, 79, 0, 124);
    cairo_pattern_add_color_stop_rgba(vig, 0.0, 0, 0, 0, 0.00);
    cairo_pattern_add_color_stop_rgba(vig, 1.0, 0, 0, 0, 0.15);
    rounded_rect_path(cr, 4, 4, 120, 120, 31);
    cairo_set_source(cr, vig);
    cairo_fill(cr);
    cairo_pattern_destroy(vig);
  }

  /* 闪电：先画一层柔化投影（细描边叠加模拟模糊），再画实体 */
  if (!small) {
    cairo_save(cr);
    cairo_translate(cr, 0, 2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (int i = 0; i < 3; i++) {
      cairo_set_line_width(cr, 3 + i * 3.5);
      cairo_set_source_rgba(cr, 0.031, 0.098, 0.290, 0.16 - i * 0.045);
      bolt_path(cr);
      cairo_fill_preserve(cr);
      cairo_stroke(cr);
    }
    cairo_restore(cr);
  }

  cairo_pattern_t *b = cairo_pattern_create_linear(50, 16, 84, 112);
  cairo_pattern_add_color_stop_rgb(b, 0.0, 1.000, 1.000, 1.000);     /* #FFFFFF */
  cairo_pattern_add_color_stop_rgb(b, 1.0, 0.894, 0.937, 1.000);     /* #E4EFFF */
  cairo_set_source(cr, b);
  cairo_set_line_width(cr, small ? 4.5 : 3.0);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  bolt_path(cr);
  cairo_fill_preserve(cr);
  cairo_stroke(cr);
  cairo_pattern_destroy(b);

  /* 内描边 */
  if (!small) {
    cairo_set_line_width(cr, 1.2);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
    rounded_rect_path(cr, 5.4, 5.4, 117.2, 117.2, 29.6);
    cairo_stroke(cr);
  }
}

/* 把托盘图标导出成 PNG（调试/商店素材用） */
gboolean uq_tray_write_icon_png(const char *path, int size) {
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(surf);
  cairo_scale(cr, (double)size / 128.0, (double)size / 128.0);
  draw_icon(cr, size <= 24);
  cairo_destroy(cr);
  cairo_status_t st = cairo_surface_write_to_png(surf, path);
  cairo_surface_destroy(surf);
  return st == CAIRO_STATUS_SUCCESS;
}

/* 生成 SNI 的 IconPixmap：a(iiay)，ARGB32 大端字节序 */
static GVariant *build_pixmaps(void) {
  static const int sizes[] = { 22, 44, 88 };
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
  for (gsize k = 0; k < G_N_ELEMENTS(sizes); k++) {
    int w = sizes[k], h = sizes[k];
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    cairo_scale(cr, (double)w / 128.0, (double)h / 128.0);
    draw_icon(cr, w <= 24);
    cairo_destroy(cr);
    cairo_surface_flush(surf);

    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    unsigned char *argb = g_malloc((gsize)w * h * 4);
    for (int y = 0; y < h; y++) {
      guint32 *row = (guint32 *)(data + y * stride);
      for (int x = 0; x < w; x++) {
        guint32 px = row[x];                /* 本机小端：AARRGGBB 内存序为 B,G,R,A */
        unsigned a = (px >> 24) & 0xff;
        unsigned r0 = (px >> 16) & 0xff, g0 = (px >> 8) & 0xff, b0 = px & 0xff;
        if (a && a < 255) {                 /* 预乘 -> 直通 */
          r0 = MIN(255u, r0 * 255 / a);
          g0 = MIN(255u, g0 * 255 / a);
          b0 = MIN(255u, b0 * 255 / a);
        }
        gsize off = ((gsize)y * w + x) * 4;
        argb[off + 0] = (unsigned char)a;
        argb[off + 1] = (unsigned char)r0;
        argb[off + 2] = (unsigned char)g0;
        argb[off + 3] = (unsigned char)b0;
      }
    }
    cairo_surface_destroy(surf);
    g_variant_builder_add(&b, "(ii@ay)", w, h,
                          g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, argb,
                                                    (gsize)w * h * 4, 1));
    g_free(argb);
  }
  return g_variant_builder_end(&b);
}

/* ---------- dbusmenu ---------- */

static GVariant *menu_props(int id) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  gboolean on = uq_autostart_enabled();
  switch (id) {
    case 0:
      g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu"));
      break;
    case MENU_OPEN:
      g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("打开 UQuick"));
      break;
    case MENU_SETTINGS:
      g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("设置…"));
      break;
    case MENU_SEP1:
    case MENU_SEP2:
      g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("separator"));
      break;
    case MENU_AUTOSTART:
      g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("开机自启"));
      g_variant_builder_add(&b, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
      g_variant_builder_add(&b, "{sv}", "toggle-state", g_variant_new_int32(on ? 1 : 0));
      break;
    case MENU_QUIT:
      g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("退出"));
      break;
    default:
      break;
  }
  g_variant_builder_add(&b, "{sv}", "visible", g_variant_new_boolean(TRUE));
  return g_variant_builder_end(&b);
}

static int menu_children(int id, int *out) {
  static const int root[] = { MENU_OPEN, MENU_SETTINGS, MENU_SEP1, MENU_AUTOSTART,
                              MENU_SEP2, MENU_QUIT };
  if (id == 0) { memcpy(out, root, sizeof(root)); return (int)G_N_ELEMENTS(root); }
  return 0;
}

static GVariant *layout_item(int id, int depth) {
  int kids[8];
  int n = menu_children(id, kids);
  GVariantBuilder children;
  g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
  if (n > 0 && depth != 0)
    for (int i = 0; i < n; i++)
      g_variant_builder_add(&children, "v", layout_item(kids[i], depth - 1));
  return g_variant_new("(i@a{sv}@av)", id, menu_props(id),
                       g_variant_builder_end(&children));
}

static void emit_layout_updated(void) {
  revision++;
  g_dbus_connection_emit_signal(bus, NULL, MENU_PATH, "com.canonical.dbusmenu",
                                "LayoutUpdated",
                                g_variant_new("(ui)", revision, 0), NULL);
}

static void menu_activate(int id) {
  switch (id) {
    case MENU_OPEN:       uq_ui_toggle(); break;
    case MENU_SETTINGS:   uq_settings_show(); break;
    case MENU_AUTOSTART: {
      GError *err = NULL;
      if (!uq_autostart_set(!uq_autostart_enabled(), &err)) {
        g_printerr("uquick: 切换开机自启失败: %s\n", err ? err->message : "未知错误");
        g_clear_error(&err);
      }
      uq_index_load();            /* 面板里的“开机自启”副标题跟着变 */
      if (uq_ui_is_visible()) uq_ui_refresh();
      emit_layout_updated();
      break;
    }
    case MENU_QUIT:       uq_quit(); break;
    default: break;
  }
}

static void on_menu_call(GDBusConnection *conn, const char *sender, const char *path,
                         const char *iface, const char *method, GVariant *params,
                         GDBusMethodInvocation *inv, gpointer data) {
  (void)conn; (void)sender; (void)path; (void)iface; (void)data;

  if (g_strcmp0(method, "GetLayout") == 0) {
    int parent = 0, depth = 0;
    GVariant *names = NULL;
    g_variant_get(params, "(ii@as)", &parent, &depth, &names);
    g_variant_unref(names);
    g_dbus_method_invocation_return_value(
        inv, g_variant_new("(u@(ia{sv}av))", revision, layout_item(parent, depth)));
    return;
  }
  if (g_strcmp0(method, "GetGroupProperties") == 0) {
    GVariant *ids = NULL;
    const char **names = NULL;
    g_variant_get(params, "(@ai^as)", &ids, &names);
    g_free(names);
    GVariantBuilder out;
    g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
    GVariantIter it;
    g_variant_iter_init(&it, ids);
    gint32 id;
    while (g_variant_iter_next(&it, "i", &id))
      g_variant_builder_add(&out, "(i@a{sv})", id, menu_props(id));
    g_variant_unref(ids);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@a(ia{sv}))",
                                                             g_variant_builder_end(&out)));
    return;
  }
  if (g_strcmp0(method, "GetProperty") == 0) {
    gint32 id = 0;
    const char *name = NULL;
    g_variant_get(params, "(i&s)", &id, &name);
    GVariant *props = menu_props(id);
    GVariant *v = g_variant_lookup_value(props, name, NULL);
    g_variant_unref(props);
    if (!v) {
      g_dbus_method_invocation_return_dbus_error(inv, "com.canonical.dbusmenu.Error",
                                                 "no such property");
      return;
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", v));
    return;
  }
  if (g_strcmp0(method, "Event") == 0) {
    gint32 id = 0;
    const char *event = NULL;
    GVariant *val = NULL;
    guint32 ts = 0;
    g_variant_get(params, "(i&s@vu)", &id, &event, &val, &ts);
    g_variant_unref(val);
    if (g_strcmp0(event, "clicked") == 0) menu_activate(id);
    else if (g_strcmp0(event, "about-to-show") == 0) emit_layout_updated();
    g_dbus_method_invocation_return_value(inv, NULL);
    return;
  }
  if (g_strcmp0(method, "EventGroup") == 0) {
    GVariant *events = NULL;
    g_variant_get(params, "(@a(isvu))", &events);
    GVariantIter it;
    g_variant_iter_init(&it, events);
    gint32 id; const char *event; GVariant *v; guint32 ts;
    while (g_variant_iter_next(&it, "(i&svu)", &id, &event, &v, &ts)) {
      if (g_strcmp0(event, "clicked") == 0) menu_activate(id);
      g_variant_unref(v);
    }
    g_variant_unref(events);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(ai)", NULL));
    return;
  }
  if (g_strcmp0(method, "AboutToShow") == 0) {
    g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
    return;
  }
  if (g_strcmp0(method, "AboutToShowGroup") == 0) {
    g_dbus_method_invocation_return_value(inv, g_variant_new("(aiai)", NULL, NULL));
    return;
  }
  g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod",
                                             method);
}

static GVariant *on_menu_prop(GDBusConnection *c, const char *sender, const char *path,
                              const char *iface, const char *prop, GError **err,
                              gpointer data) {
  (void)c; (void)sender; (void)path; (void)iface; (void)err; (void)data;
  if (g_strcmp0(prop, "Version") == 0) return g_variant_new_uint32(3);
  if (g_strcmp0(prop, "TextDirection") == 0) return g_variant_new_string("ltr");
  if (g_strcmp0(prop, "Status") == 0) return g_variant_new_string("normal");
  if (g_strcmp0(prop, "IconThemePath") == 0) return g_variant_new_strv(NULL, 0);
  return NULL;
}

/* ---------- StatusNotifierItem ---------- */

static GVariant *on_sni_prop(GDBusConnection *c, const char *sender, const char *path,
                             const char *iface, const char *prop, GError **err,
                             gpointer data) {
  (void)c; (void)sender; (void)path; (void)iface; (void)err; (void)data;
  if (g_strcmp0(prop, "Category") == 0) return g_variant_new_string("ApplicationStatus");
  if (g_strcmp0(prop, "Id") == 0) return g_variant_new_string("uquick");
  if (g_strcmp0(prop, "Title") == 0) return g_variant_new_string("UQuick");
  if (g_strcmp0(prop, "Status") == 0) return g_variant_new_string("Active");
  /* 故意留空：ubuntu-appindicators 里 name 优先于 pixmap，而 snap 的图标不在
   * 宿主机图标主题里，给名字只会查到 "image-missing"。留空才会回落到 IconPixmap。 */
  if (g_strcmp0(prop, "IconName") == 0) return g_variant_new_string("");
  if (g_strcmp0(prop, "IconPixmap") == 0) return build_pixmaps();
  if (g_strcmp0(prop, "Menu") == 0) return g_variant_new_object_path(MENU_PATH);
  if (g_strcmp0(prop, "ItemIsMenu") == 0) return g_variant_new_boolean(TRUE);
  if (g_strcmp0(prop, "ToolTip") == 0) {
    GVariantBuilder pix;
    g_variant_builder_init(&pix, G_VARIANT_TYPE("a(iiay)"));
    return g_variant_new("(s@a(iiay)ss)", "io.github.suveng.uquick",
                         g_variant_builder_end(&pix), "UQuick", "点击打开启动器");
  }
  return NULL;
}

static void on_sni_call(GDBusConnection *conn, const char *sender, const char *path,
                        const char *iface, const char *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer data) {
  (void)conn; (void)sender; (void)path; (void)iface; (void)params; (void)data;
  if (g_strcmp0(method, "Activate") == 0 || g_strcmp0(method, "SecondaryActivate") == 0) {
    uq_ui_toggle();
  } else if (g_strcmp0(method, "ContextMenu") == 0) {
    emit_layout_updated();
  }
  g_dbus_method_invocation_return_value(inv, NULL);
}

/* ---------- 注册 ---------- */

/* 向 watcher 注册；GNOME Shell 重启/扩展重载后 watcher 会换一个，需要重新注册 */
static void register_with_watcher(void) {
  if (!bus) return;
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_sync(
      bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
      "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
      g_variant_new("(s)", SNI_PATH), NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
  if (r) g_variant_unref(r);
  if (!r && err) {
    if (g_getenv("UQUICK_TRACE"))
      g_printerr("uquick: 注册托盘失败（没有 appindicator 扩展？）: %s\n", err->message);
    g_clear_error(&err);
  }
}

/* watcher 不在 -> 出现（扩展启用 / shell 重启）时补注册一次 */
static void on_watcher_owner(GDBusConnection *c, const char *sender, const char *path,
                             const char *iface, const char *signal, GVariant *params,
                             gpointer data) {
  (void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)data;
  const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
  g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
  if (g_strcmp0(name, "org.kde.StatusNotifierWatcher") != 0) return;
  if (new_owner && *new_owner) register_with_watcher();
}

gboolean uq_tray_init(GError **err) {
  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, err);
  if (!bus) return FALSE;

  GDBusNodeInfo *sni_node = g_dbus_node_info_new_for_xml(sni_xml, err);
  if (!sni_node) return FALSE;
  GDBusNodeInfo *menu_node = g_dbus_node_info_new_for_xml(menu_xml, err);
  if (!menu_node) return FALSE;

  static const GDBusInterfaceVTable sni_vt = { on_sni_call, on_sni_prop, NULL, { 0 } };
  static const GDBusInterfaceVTable menu_vt = { on_menu_call, on_menu_prop, NULL, { 0 } };

  sni_reg = g_dbus_connection_register_object(bus, SNI_PATH, sni_node->interfaces[0],
                                              &sni_vt, NULL, NULL, err);
  menu_reg = g_dbus_connection_register_object(bus, MENU_PATH, menu_node->interfaces[0],
                                              &menu_vt, NULL, NULL, err);
  g_dbus_node_info_unref(sni_node);
  g_dbus_node_info_unref(menu_node);
  if (!sni_reg || !menu_reg) return FALSE;

  sni_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", (int)getpid());
  GVariant *r = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "RequestName", g_variant_new("(su)", sni_name, 0u), NULL,
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, err);
  if (r) g_variant_unref(r);

  /* 注册到 watcher；参数用对象路径（社区惯例，避免与 brute-force 扫描算出两个 id） */
  register_with_watcher();
  g_dbus_connection_signal_subscribe(
      bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
      "/org/freedesktop/DBus", "org.kde.StatusNotifierWatcher",
      G_DBUS_SIGNAL_FLAGS_NONE, on_watcher_owner, NULL, NULL);
  return TRUE;
}

gboolean uq_tray_active(void) { return sni_reg != 0; }

void uq_tray_update(void) {
  if (bus) emit_layout_updated();
}
