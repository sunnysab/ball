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

# The user name becomes part of a unit instance name and of the drop-in path.
case "$user" in
  '' | *[!A-Za-z0-9_.-]*)
    echo "invalid user '$user': only letters, digits, '_', '.' and '-' are allowed" >&2
    exit 2 ;;
esac

# Both paths are written verbatim into a unit file, so they must be absolute
# and free of control characters.
check_path() {
  case "$2" in
    /*) ;;
    *) echo "$1 must be an absolute path: $2" >&2; exit 2 ;;
  esac
  case "$2" in
    *[[:cntrl:]]*) echo "$1 contains a control character" >&2; exit 2 ;;
  esac
}
check_path --prefix "$prefix"
if [ -n "$collector_socket" ]; then
  check_path --collector-socket "$collector_socket"
fi

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${build_dir:-$here/build/release}
unit=ball@$user.service
autostart_file=$HOME/.config/autostart/ball.desktop
# External collector mode overrides the unit's ExecStart through a drop-in,
# which a later `cmake --install` never touches.
dropin_dir=/etc/systemd/system/$unit.d
dropin_file=$dropin_dir/collector.conf

# A drop-in belongs to this script only when it is a regular file carrying the
# exact marker line; anything else is someone else's configuration.
dropin_exists() {
  sudo test -e "$dropin_file" || sudo test -L "$dropin_file"
}
dropin_is_mine() {
  if sudo test -L "$dropin_file" || ! sudo test -f "$dropin_file"; then
    return 1
  fi
  sudo grep -qx '# X-Ball-Managed=true' "$dropin_file"
}
# systemd does not run ExecStart through a shell, so quoting is the whole
# protection: escape the quote characters and the % / $ markers.
unit_arg() {
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/%/%%/g' -e 's/\$/$$/g'
}
write_dropin() {
  exec_start="\"$(unit_arg "$prefix/bin/ball")\" --daemon --socket /run/ball-%i/gui.sock"
  exec_start="$exec_start --collector-socket \"$(unit_arg "$collector_socket")\""
  tmp=$(mktemp) || exit 1
  # The local copy goes away on success and on failure alike.
  trap 'rm -f "$tmp"' EXIT
  trap 'exit 1' HUP INT TERM
  {
    echo '# X-Ball-Managed=true'
    echo '[Service]'
    echo 'ExecStart='
    # printf, not echo: dash would interpret the backslashes of the escaped path.
    printf '%s\n' "ExecStart=$exec_start"
  } > "$tmp"
  sudo install -D -m 0644 "$tmp" "$dropin_file"
  rm -f "$tmp"
  trap - EXIT HUP INT TERM
}

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
  # Only a drop-in this script wrote is removed; a custom file stays, since it
  # is the only record of how that instance finds its collector.
  if dropin_exists; then
    if dropin_is_mine; then
      sudo rm -f "$dropin_file"
      sudo rmdir "$dropin_dir" 2>/dev/null || true
    else
      echo "kept $dropin_file: it was not written by this script" >&2
    fi
  fi
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
  echo 'systemd is required: ball runs its backend as a systemd service instance' >&2
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

# Refuse a mode switch before touching anything when the drop-in at our path is
# not ours: spawn mode must not claim a switch it cannot make.
if dropin_exists && ! dropin_is_mine; then
  echo "$dropin_file already exists and was not written by this script" >&2
  if [ -n "$collector_socket" ]; then
    echo "move it aside to use --collector-socket" >&2
  else
    echo "this instance keeps using the collector configured there" >&2
  fi
  exit 1
fi

# --- build ---------------------------------------------------------------
if [ "$build" = yes ]; then
  set -- -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$prefix" -DBUILD_TESTING=OFF
  # Bundle the collector unless the caller wants an external one.
  if [ -n "$collector_socket" ]; then
    echo "using the external collector at $collector_socket"
  elif [ -x "$here/build/socktrail-new" ]; then
    set -- "$@" "-DBALL_SOCKTRAIL_BINARY=$here/build/socktrail-new"
  elif [ -x "$here/build/socktrail" ]; then
    set -- "$@" "-DBALL_SOCKTRAIL_BINARY=$here/build/socktrail"
  else
    echo "no collector binary found in $here/build; pass --collector-socket PATH" >&2
    echo "for an external collector, or build socktrail first" >&2
    exit 1
  fi
  cmake -S "$here" -B "$build_dir" -G Ninja "$@"
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

# An external collector means the packaged unit must not spawn its own; the
# drop-in overrides ExecStart and survives a reinstall of the template.
if [ -n "$collector_socket" ]; then
  echo "installing $dropin_file to subscribe to $collector_socket"
  write_dropin
elif dropin_exists; then
  echo "removing $dropin_file to spawn the bundled collector again"
  sudo rm -f "$dropin_file"
  sudo rmdir "$dropin_dir" 2>/dev/null || true
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
