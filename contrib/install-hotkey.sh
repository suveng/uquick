#!/usr/bin/env bash
# install-hotkey.sh — 给 UQuick 装 GNOME 自定义热键（以及（可选）开机自启）
#
#   ./install-hotkey.sh                       # 默认组合 <Super>space
#   ./install-hotkey.sh -b '<Alt>space'       # 指定组合
#   ./install-hotkey.sh -n                    # 预演，只打印不写入
#   ./install-hotkey.sh -r                    # 卸载热键与自启
#
# 为什么必须这么绕：Wayland 下应用无法自己抓全局按键（GNOME 49+ 的 mutter
# 只在 X11 窗口持有焦点时转发按键抓取）。可靠做法是写 GNOME 自定义快捷键，
# 由 gnome-settings-daemon 拉起 `uquick --toggle`：
#   按键 -> gsd-media-keys -> exec uquick --toggle
#        -> unix socket -> 常驻进程 present 已有窗口（1 帧内上屏）
set -euo pipefail

BINDING="<Super>space"
DRY=0
REMOVE=0
FAST=0
SLOT="custom-uquick"

while getopts ":b:nr:fh" o; do
  case "$o" in
    b) BINDING="$OPTARG" ;;
    n) DRY=1 ;;
    f) FAST=1 ;;
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

# 冲突提示：该组合是否已被 GNOME 内置键盘绑定占用
case "$BINDING" in
  '<Alt>space')
    v="$(gsettings get org.gnome.desktop.wm.keybindings activate-window-menu 2>/dev/null || true)"
    [ "$v" = "['<Alt>space']" ] && {
      say "注意：<Alt>space 同时是 GNOME 的窗口菜单键，可能双触发。"
      say "      建议 -b '<Super>space'，或先 gsettings set org.gnome.desktop.wm.keybindings activate-window-menu \"[]\"" ; } ;;
  '<Super>space')
    v="$(gsettings get org.gnome.desktop.wm.keybindings switch-input-source 2>/dev/null || true)"
    case "$v" in *"'<Super>space'"*)
      say "注意：<Super>space 也是 GNOME 的输入源切换键（仅当配置了多种输入法时才冲突）。" ;;
    esac ;;
esac

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
