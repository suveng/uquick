#!/usr/bin/env bash
# mkstore-shots.sh — 离屏生成 App Center 用的 1920x1080 商店截图
#
# 不拍真实屏幕（避免把用户窗口/隐私内容带进商店素材）：
#   Xvfb 起 1920x1080 空屏 -> 只截 uquick 自己的窗口 -> 裁掉纯白角
#   -> 合成到自绘渐变背景上（带投影）
#
#   ./contrib/mkstore-shots.sh [-o 输出目录]
set -euo pipefail

OUT="store"
while getopts ":o:h" o; do
  case "$o" in
    o) OUT="$OPTARG" ;;
    h) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) exit 2 ;;
  esac
done
mkdir -p "$OUT"
TMP="$(mktemp -d)"
BIN="${UQUICK_BIN:-$PWD/build/uquick}"
[ -x "$BIN" ] || { echo "找不到 $BIN（先 ninja -C build）" >&2; exit 1; }

cleanup() {
  pkill -x uquick 2>/dev/null || true
  pkill -x Xvfb 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT

pkill -x Xvfb 2>/dev/null || true
sleep 0.3
setsid Xvfb :99 -screen 0 1920x1080x24 -nolisten tcp >/dev/null 2>&1 &
sleep 1.2

export DISPLAY=:99 GDK_BACKEND=x11 GSK_RENDERER=cairo GDK_SCALE=2   # 2x 渲染更清晰
export UQUICK_NO_TRAY=1   # 离屏截图别去抢真实托盘的注册
setsid "$BIN" --daemon >/dev/null 2>&1 &
sleep 1.5

# 背景：深色渐变 + 一条水平分隔，纯自绘
convert -size 1920x1080 gradient:'#22303c-#0d151d' \
  -fill '#ffffff08' -draw "line 0,540 1920,540" "$TMP/bg.png"

shot() { # $1=查询 $2=输出文件
  "$BIN" --query "$1"
  sleep 0.8
  command -v xdotool >/dev/null && xdotool mousemove 5 5   # 别让 tooltip 入镜
  sleep 0.3
  import -window root "$TMP/raw.png"
  convert "$TMP/raw.png" -trim +repage "$TMP/win.png"

  # X11 无合成器，圆角外的透明区会截成白色 -> 用同半径圆角遮罩裁掉
  local w h
  w=$(identify -format %w "$TMP/win.png")
  h=$(identify -format %h "$TMP/win.png")
  convert -size "${w}x${h}" xc:black -fill white \
    -draw "roundrectangle 0,0,$((w-1)),$((h-1)),32,32" "$TMP/mask.png"
  convert "$TMP/win.png" "$TMP/mask.png" -alpha off -compose CopyOpacity -composite \
    "$TMP/round.png"

  convert "$TMP/round.png" \
    \( +clone -background black -shadow 55x18+0+12 \) +swap \
    -background none -layers merge +repage "$TMP/shadow.png"

  convert "$TMP/bg.png" "$TMP/shadow.png" -gravity north -geometry +0+150 \
    -composite -strip -quality 92 "$2"
  echo "生成 $2"
}

shot "web"              "$OUT/screenshot-1.png"
shot "1+2*3+sqrt(16)"   "$OUT/screenshot-2.png"
shot "term"             "$OUT/screenshot-3.png"

# 设置窗口
"$BIN" --settings
sleep 1
command -v xdotool >/dev/null && xdotool mousemove 5 5
sleep 0.3
import -window root "$TMP/raw4.png"
convert "$TMP/raw4.png" -trim +repage "$TMP/win4.png"
w=$(identify -format %w "$TMP/win4.png"); h=$(identify -format %h "$TMP/win4.png")
convert -size "${w}x${h}" xc:black -fill white \
  -draw "roundrectangle 0,0,$((w-1)),$((h-1)),24,24" "$TMP/mask4.png"
convert "$TMP/win4.png" "$TMP/mask4.png" -alpha off -compose CopyOpacity -composite "$TMP/round4.png"
convert "$TMP/round4.png" \( +clone -background black -shadow 55x18+0+12 \) +swap \
  -background none -layers merge +repage "$TMP/shadow4.png"
convert "$TMP/bg.png" "$TMP/shadow4.png" -gravity center -geometry +0+40 \
  -composite -strip -quality 92 "$OUT/screenshot-4.png"
echo "生成 $OUT/screenshot-4.png"
ls -lh "$OUT"/screenshot-*.png
