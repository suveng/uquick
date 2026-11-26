#!/usr/bin/env bash
# install-hotkey.sh — 给 UQuick 装 GNOME 自定义热键（以及（可选）开机自启）
#
#   ./install-hotkey.sh                       # 默认组合 <Super>slash
#   ./install-hotkey.sh -b '<Ctrl><Alt>space' # 指定组合
#   ./install-hotkey.sh -F -b '<Super>space'  # 已知会被 GNOME 抢走的组合，仍然强制写入
#   ./install-hotkey.sh -n                    # 预演，只打印不写入
#   ./install-hotkey.sh -r                    # 卸载热键与自启
#
# 为什么必须这么绕：Wayland 下应用无法自己抓全局按键（GNOME 49+ 的 mutter
# 只在 X11 窗口持有焦点时转发按键抓取）。可靠做法是写 GNOME 自定义快捷键，
# 由 gnome-settings-daemon 拉起 `uquick --toggle`：
#   按键 -> gsd-media-keys -> exec uquick --toggle
#        -> unix socket -> 常驻进程 present 已有窗口（1 帧内上屏）
set -euo pipefail

# 默认不要用 <Super>space / <Alt>space：实测前者被 GNOME 的输入源切换抢走、
# 后者是窗口菜单键，都会导致"按了没反应"（用 uinput 注入真实按键验证过）。
BINDING="<Super>slash"
DRY=0
REMOVE=0
FAST=0
FORCE=0
SLOT="custom-uquick"

while getopts ":b:nr:fFh" o; do
  case "$o" in
    b) BINDING="$OPTARG" ;;
    n) DRY=1 ;;
    f) FAST=1 ;;
    F) FORCE=1 ;;
    r) REMOVE=1 ;;
    h) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数：-$OPTARG（-h 看用法）" >&2; exit 2 ;;
  esac
done

say() { printf '%s\n' "$*"; }
run() { if [ "$DRY" = 1 ]; then say "[dry-run] $*"; else "$@"; fi; }

MEDIA="org.gnome.settings-daemon.plugins.media-keys"
BASE="/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings"
KEY_PATH="$BASE/$SLOT/"
AUTOSTART_SYS="/etc/xdg/autostart/uquick-daemon.desktop"
AUTOSTART_USER="$HOME/.config/autostart/uquick-daemon.desktop"

# 用 python3 处理 gsettings 的 "['a', 'b']" 字面量（GVariant 打印格式）
list_edit() {  # $1=当前值 $2=add|del $3=路径
  python3 - "$1" "$2" "$3" <<'PY'
import ast, sys
raw, op, path = sys.argv[1], sys.argv[2], sys.argv[3]
raw = raw.strip()
lst = [] if raw in ("", "@as []", "[]") else ast.literal_eval(raw)
lst = [p for p in lst if p]
if op == "add":
    if path not in lst:
        lst.append(path)
else:
    lst = [p for p in lst if p != path]
print("[" + ", ".join("'%s'" % p for p in lst) + "]")
PY
}

list="$(gsettings get "$MEDIA" custom-keybindings)"

if [ "$REMOVE" = 1 ]; then
  say "移除热键槽位：$KEY_PATH"
  run gsettings set "$MEDIA" custom-keybindings "$(list_edit "$list" del "$KEY_PATH")"
  run gsettings reset-recursively "$MEDIA.custom-keybinding:$KEY_PATH" 2>/dev/null || true
  if [ -f "$AUTOSTART_USER" ]; then
    say "移除用户自启项：$AUTOSTART_USER"
    run rm -f "$AUTOSTART_USER"
  fi
  exit 0
fi

# 找可执行文件：snap 优先，其次系统安装，最后本地构建
BIN=""
for cand in /snap/bin/uquick /usr/bin/uquick "$PWD/build/uquick"; do
  [ -x "$cand" ] && { BIN="$cand"; break; }
done
if [ -z "$BIN" ]; then
  echo "找不到 uquick（先 ninja -C build，或 sudo meson install -C build）" >&2
  exit 1
fi
say "可执行文件：$BIN"

# 冲突检查：GNOME 自己的键位会抢在自定义快捷键前面（实测按了没反应）
claim_check() {
  local b="$1" key v
  for key in activate-window-menu switch-input-source switch-input-source-backward \
             toggle-overview panel-run-dialog; do
    v="$(gsettings get org.gnome.desktop.wm.keybindings "$key" 2>/dev/null || true)"
    case "$v" in *"'$b'"*) printf '%s=%s' "$key" "$v"; return 0 ;; esac
  done
  return 1
}
if claim=$(claim_check "$BINDING"); then
  if [ "$FORCE" = 1 ]; then
    say "警告：$BINDING 已被 GNOME 占用（$claim），强制写入后大概率按了没反应。"
  else
    say "错误：$BINDING 已被 GNOME 内置绑定占用：$claim"
    say "      实测这种组合会被 GNOME 先吃掉（例：<Super>space 是输入源切换），"
    say "      换一个组合，例如 -b '<Super>slash' / -b '<Ctrl><Alt>space'；"
    say "      确实要强写就加 -F。"
    exit 1
  fi
fi

if printf '%s' "$list" | grep -qF "$KEY_PATH"; then
  say "槽位已存在：$KEY_PATH"
else
  say "注册槽位：$KEY_PATH"
  run gsettings set "$MEDIA" custom-keybindings "$(list_edit "$list" add "$KEY_PATH")"
fi

# 极速客户端：classic 快照的 socket 就在宿主机 /run/user/1000/ 上，
# 用 nc 直连省掉每次 snap run 的 ~55ms；连不上再回落到正式入口（会自启守护进程）
SOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/uquick.sock"
CMD="$BIN --toggle"
if [ "$FAST" = 1 ] && command -v nc >/dev/null; then
  CMD="sh -c 'printf toggle | nc -U $SOCK 2>/dev/null || exec $BIN --toggle'"
  say "客户端：快路径（nc -U $SOCK，约 10 ms）"
fi

run gsettings set "$MEDIA.custom-keybinding:$KEY_PATH" name 'UQuick 启动器'
run gsettings set "$MEDIA.custom-keybinding:$KEY_PATH" command "$CMD"
run gsettings set "$MEDIA.custom-keybinding:$KEY_PATH" binding "$BINDING"

# 托盘图标：shell 的图标主题要能按名字查到（classic snap 的图标不在宿主主题里）
ICON_REL="usr/share/icons/hicolor/scalable/apps/io.github.suveng.uquick.svg"
ICON_SRC=""
for c in "$(dirname "$0")/../data/icons/hicolor/scalable/apps/io.github.suveng.uquick.svg" \
         "/snap/uquick/current/$ICON_REL" "/usr/$ICON_REL"; do
  [ -f "$c" ] && { ICON_SRC="$c"; break; }
done
ICON_DST="$HOME/.local/share/icons/hicolor/scalable/apps/io.github.suveng.uquick.svg"
if [ -n "$ICON_SRC" ]; then
  say "托盘图标：$ICON_DST"
  if [ "$DRY" = 1 ]; then
    say "[dry-run] 复制图标并更新缓存"
  else
    mkdir -p "$(dirname "$ICON_DST")"
    cp -f "$ICON_SRC" "$ICON_DST"
    gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" >/dev/null 2>&1 || true
  fi
else
  say "提示：没找到图标源文件，托盘可能显示缺失图标（可手动 cp 到 $ICON_DST）"
fi

# 常驻进程自启：deb 安装自带系统级自启；snap 版写用户级
if [ -f "$AUTOSTART_SYS" ]; then
  say "自启：已由 $AUTOSTART_SYS 提供"
else
  say "自启：$AUTOSTART_USER"
  if [ "$DRY" = 1 ]; then
    say "[dry-run] 写入 $AUTOSTART_USER"
  else
    mkdir -p "$HOME/.config/autostart"
    cat > "$AUTOSTART_USER" <<EOF
[Desktop Entry]
Type=Application
Name=UQuick Daemon
Comment=保持 UQuick 常驻，热键唤出零延迟
Exec=$BIN --daemon
Terminal=false
NoDisplay=true
X-GNOME-Autostart-enabled=true
EOF
  fi
fi

say ""
say "热键 = $(gsettings get "$MEDIA.custom-keybinding:$KEY_PATH" binding 2>/dev/null || echo "$BINDING")"
say "命令 = $(gsettings get "$MEDIA.custom-keybinding:$KEY_PATH" command 2>/dev/null || echo "$BIN --toggle")"
run "$BIN" --toggle
say "done"
