# classic 权限申请文案（发到 forum.snapcraft.io → store-requests → classic-confinement）

> 审核通过前，`snapcraft upload --release=edge uquick` 只会停在 `will need manual review`。

## 标题

Classic confinement request for uquick (Ubuntu native launcher / command palette)

## 正文

**Name of the snap**: `uquick`

**Store URL**: https://snapcraft.io/uquick (registered; no released revisions yet)

**Source**: https://github.com/suveng/uquick (GPL-3.0-or-later, ~1.6k lines of C)

**What it does**

UQuick is a native (C + GTK4) launcher / command palette for the Ubuntu desktop,
the local replacement for uTools-style Electron launchers. Pressing one hotkey opens
a search box that can:

1. enumerate and launch **every installed host application** by reading
   `/usr/share/applications/**` and the other `XDG_DATA_DIRS` directories, then
   `exec` the chosen `.desktop` entry (`g_app_info_launch`, incl. `Terminal=true`);
2. run **user-configured commands** from `~/.config/uquick/commands.tsv`
   (`name<TAB>command`), and arbitrary shell one-liners entered directly with a `>`
   prefix — i.e. it is a user-driven command runner, like rofi/albert/dmenu;
3. offer system actions (lock/suspend/logout/reboot/poweroff, settings panels) and
   an inline calculator.

**Why strict confinement cannot work**

A launcher's whole job is executing arbitrary host programs, which strict snaps
cannot do — this is the case the classic-confinement docs describe as the primary
reason for classic. Measured on Ubuntu 26.04:

```
$ snap run --shell shot-screenshot.shot -c 'ls /usr/bin | wc -l; ls /usr/share/applications'
468                              # host has 2850 binaries
mimeapps.list  xdg-open.desktop  # host applications/ is not visible at all
$ snap run --shell shot-screenshot.shot -c 'gtk-launch org.gnome.Nautilus'
gtk-launch: no such application
```

Inside a strict snap, `/usr` is the base snap. There is no way to read the host's
`.desktop` catalogue or to `exec` host binaries, and there is no portal that launches
arbitrary desktop entries (`org.freedesktop.portal.DynamicLauncher` only launches
launchers previously installed *through* the portal, one authorization UI per entry).
So a strict uquick would be an empty search box — the requested category in
"Reviewing classic confinement snaps" is:

- *terminal emulators, multiplexers and shells*
- *running arbitrary command (esp if user-configurable …)*

**Risk-reduction measures**

- no `plugs`/`slots` are declared (as required for classic);
- the snap ships only a 84 KB binary and links the host's GTK4 — no runtime bundled,
  no network access used, no daemon running as root, no writes outside
  `$XDG_CONFIG_HOME`/`$XDG_STATE_HOME`;
- launches applications with `GDesktopAppInfo`/`g_app_info_launch` (i.e. exactly what
  the desktop's own app grid does), never with elevated privileges; the built-in
  system actions call `loginctl`, `systemctl suspend`, `gnome-session-quit` as the
  invoking user;
- the `>` shell mode is explicit and opt-in per keystroke, same as a terminal;
- source is public and the app is reproducible with `snapcraft pack --destructive-mode`.

**Snapcraft / build**

```yaml
confinement: classic
base: core26
apps:
  uquick:
    command: usr/bin/uquick
    desktop: usr/share/applications/io.github.suveng.uquick.desktop
parts:
  uquick:
    plugin: meson
    parse-info: [usr/share/metainfo/io.github.suveng.uquick.metainfo.xml]
```

Happy to adjust anything (e.g. drop the `>` shell mode) if that helps the review.
