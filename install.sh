#!/bin/sh
# Build and install ball, then set up its systemd backend and the per-user GUI
# autostart entry.
#
# ball has two parts with separate lifecycles:
#   - the backend daemon (root, systemd template ball@<user>.service)
#   - the GUI HUD (the desktop user's session)
# The backend is what must start at boot; the GUI autostart entry only decides
# whether the floating window comes up when the user logs in.
#
# Usage:
#   ./install.sh                      build, install for the current user
#   ./install.sh --user alice         set up the backend for another user
#   ./install.sh --no-build           use the existing build directory
#   ./install.sh --prefix /usr        install prefix (default /usr/local)
#   ./install.sh --collector-socket PATH
#                                     subscribe to an already running collector
#                                     instead of the bundled socktrail
#   ./install.sh --no-enable          install files only, do not enable services
#   ./install.sh --no-gui-autostart   leave the GUI login autostart untouched
#   ./install.sh --uninstall          stop and remove everything this script added
set -eu

prefix=/usr/local
user=$(id -un)
build=yes
enable=yes
gui_autostart=yes
collector_socket=
uninstall=no

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) prefix=$2; shift 2 ;;
    --user) user=$2; shift 2 ;;
    --build-dir) build_dir=$2; shift 2 ;;
    --collector-socket) collector_socket=$2; shift 2 ;;
    --no-build) build=no; shift ;;
    --no-enable) enable=no; shift ;;
    --no-gui-autostart) gui_autostart=no; shift ;;
    --uninstall) uninstall=yes; shift ;;
    -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${build_dir:-$here/build/release}
unit=ball@$user.service
autostart_file=$HOME/.config/autostart/ball.desktop

if [ "$(uname -s)" != Linux ]; then
  echo 'ball supports Linux only' >&2
  exit 1
fi

# --- uninstall -----------------------------------------------------------
if [ "$uninstall" = yes ]; then
  if [ "$(id -u)" -eq 0 ] && command -v systemctl >/dev/null 2>&1; then
    systemctl disable --now "$unit" 2>/dev/null || true
  else
    sudo systemctl disable --now "$unit" 2>/dev/null || true
  fi
  # Remove only the files this project installs.
  sudo rm -f "$prefix/lib/systemd/system/ball@.service"
  sudo rm -f "$prefix/share/applications/ball.desktop"
  sudo rm -f "$prefix/share/icons/hicolor/scalable/apps/ball.svg"
  sudo rm -f "$prefix/bin/ball"
  sudo rm -f "$prefix/libexec/ball/socktrail"
  sudo rmdir "$prefix/libexec/ball" 2>/dev/null || true
  # The GUI autostart entry is the user's own file; only a managed one is removed.
  if [ -f "$autostart_file" ] && grep -q '^X-Ball-Managed=true' "$autostart_file"; then
    rm -f "$autostart_file"
  fi
  command -v systemctl >/dev/null 2>&1 && sudo systemctl daemon-reload || true
  echo "removed ball from $prefix and disabled $unit"
  exit 0
fi

# --- prerequisites -------------------------------------------------------
if ! command -v systemctl >/dev/null 2>&1; then
  echo 'systemd is required: ball installs two systemd units' >&2
  exit 1
fi
# Group=%i needs a private group named after the user; without it the unit
# fails at start and the GUI cannot reach the socket.
if ! getent group "$user" >/dev/null 2>&1; then
  echo "no group named '$user' exists." >&2
  echo "create it first:  sudo groupadd $user && sudo usermod -aG $user $user" >&2
  echo "(then log out and back in so the session picks up the group)" >&2
  exit 1
fi

# --- build ---------------------------------------------------------------
if [ "$build" = yes ]; then
  cmake_args="-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$prefix -DBUILD_TESTING=OFF"
  # Bundle the collector unless the caller wants an external one.
  if [ -n "$collector_socket" ]; then
    echo "using the external collector at $collector_socket"
  elif [ -x "$here/build/socktrail-new" ]; then
    cmake_args="$cmake_args -DBALL_SOCKTRAIL_BINARY=$here/build/socktrail-new"
  elif [ -x "$here/build/socktrail" ]; then
    cmake_args="$cmake_args -DBALL_SOCKTRAIL_BINARY=$here/build/socktrail"
  else
    echo "no collector binary found in $here/build; pass --collector-socket PATH" >&2
    echo "for an external collector, or build socktrail first" >&2
    exit 1
  fi
  # shellcheck disable=SC2086
  cmake -S "$here" -B "$build_dir" -G Ninja $cmake_args
  cmake --build "$build_dir"
fi

# --- install files -------------------------------------------------------
if [ ! -f "$build_dir/cmake_install.cmake" ]; then
  echo "no configured build in $build_dir; drop --no-build or run cmake first" >&2
  exit 1
fi
# --prefix is explicit: the configured build may target another prefix, and
# installing the files somewhere else is exactly what a staged test wants.
sudo cmake --install "$build_dir" --prefix "$prefix"

# An external collector means the packaged unit must not spawn its own.
if [ -n "$collector_socket" ]; then
  unit_path=$prefix/lib/systemd/system/ball@.service
  echo "rewriting ExecStart to subscribe to $collector_socket"
  sudo sed -i "s|--socktrail-bin \"[^\"]*\"|--collector-socket \"$collector_socket\"|" "$unit_path"
fi

command -v systemctl >/dev/null 2>&1 && sudo systemctl daemon-reload

# --- backend service (starts at boot) ------------------------------------
if [ "$enable" = yes ]; then
  sudo systemctl enable --now "$unit"
  echo "backend enabled at boot: $unit"
else
  echo "backend installed but not enabled; start it with: sudo systemctl enable --now $unit"
fi

# --- GUI autostart (this user's login) -----------------------------------
if [ "$gui_autostart" = yes ]; then
  gui_exec=$prefix/bin/ball
  mkdir -p "$(dirname "$autostart_file")"
  # Same keys the in-app toggle writes, so the menu reflects this entry.
  cat > "$autostart_file" <<EOF
[Desktop Entry]
Type=Application
Name=ball 流量悬浮窗
Exec="$gui_exec"
Terminal=false
OnlyShowIn=KDE;
X-Ball-Managed=true
EOF
  echo "GUI autostart written: $autostart_file"
fi

echo
echo "done. Verify with:"
echo "  systemctl status $unit"
echo "  journalctl -u $unit -n 20 --no-pager"
echo "  ls -l /run/ball-$user/gui.sock"
