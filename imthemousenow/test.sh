#!/bin/bash
# Does this build still speak the language imthemousenow speaks to it?
#
# imthemousenow drives wl-kbptr through three things, and each fails silently:
#
#   - capability probes. The wrapper greps the binary for a string per patched
#     feature (`binary_knows` in bin/imthemousenow-lib.sh, and each ACTION's
#     `requires` in config.default.toml). A string renamed here turns the
#     feature off there, with no error, on every machine.
#   - options. wl-kbptr rejects a whole config over one key it does not know,
#     so an option the wrapper passes that this build no longer accepts takes
#     every chord down, not just its own feature.
#   - arguments. --drag and --hold are how the drag and hold ACTIONs work at
#     all; a changed format is a drag that never happens.
#
# None of it needs a compositor. wl-kbptr parses every argument and loads its
# whole configuration before it connects to Wayland, so pointed at a display
# that does not exist, a command it accepts fails on the connect and one it
# rejects fails before it. What is not covered is behaviour on screen -- the
# peek fading, the hold being steered, the second click -- which is still
# checked by hand on the running compositor.
#
#   imthemousenow/test.sh [IMTHEMOUSENOW_CHECKOUT]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PLUGIN="${1:-$ROOT/../imthemousenow}"
BUILD="$HERE/build"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fails=0
ok() { printf 'ok    %s\n' "$1"; }
no() { printf 'FAIL  %s\n' "$1"; fails=$((fails + 1)); }

# --- 0. build ------------------------------------------------------------------
# With OpenCV, as imthemousenow's install builds it by default, so the binary
# under test is the one people run.
if [[ ! -d $BUILD ]]; then
  meson setup "$BUILD" "$ROOT" -Dopencv=enabled >"$WORK/meson.log" 2>&1 ||
    { cat "$WORK/meson.log"; echo "FAIL  meson setup"; exit 1; }
fi
meson compile -C "$BUILD" >"$WORK/build.log" 2>&1 ||
  { tail -30 "$WORK/build.log"; echo "FAIL  build"; exit 1; }
BIN="$BUILD/wl-kbptr"
ok "builds"

# A home and runtime dir of this test's own: wl-kbptr reads
# ~/.config/wl-kbptr/config when not given -c, so without this the result
# would depend on whoever runs it. WAYLAND_DISPLAY names a socket that cannot
# exist, so nothing here ever reaches a real compositor.
export HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" \
  XDG_RUNTIME_DIR="$WORK/run" WAYLAND_DISPLAY="imthemousenow-test-no-such-display"
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"
unset WL_KBPTR_KEY_CHANNEL

CONNECT="Failed to connect to Wayland compositor"

# Accepted: it got all the way to the connect.
accepts() {
  local label="$1"; shift
  local err
  err="$("$BIN" "$@" </dev/null 2>&1 >/dev/null)"
  if [[ $err == *"$CONNECT"* ]]; then ok "$label"; else
    no "$label"; printf '        %s\n' "wl-kbptr $*" "${err:-<no output>}" | head -6
  fi
}

# Rejected: it stopped before the connect, with an error.
rejects() {
  local label="$1"; shift
  local err
  err="$("$BIN" "$@" </dev/null 2>&1 >/dev/null)"
  if [[ $err != *"$CONNECT"* && -n $err ]]; then ok "$label"; else
    no "$label"; printf '        %s\n' "wl-kbptr $*" "${err:-<no output>}" | head -6
  fi
}

# --- 1. the harness can tell the difference -------------------------------------
# Without these two, every "accepts" below could be passing because nothing is
# ever rejected.
accepts "a stock option is accepted" -o modes=tile,click
rejects "an option no build has ever had is rejected" -o general.imthemousenow_no_such_key=1

# --- 2. capability probes ----------------------------------------------------------
# The strings imthemousenow looks for, and what it turns on when it finds them.
# Kept here as a list, and checked against the checkout below when there is
# one, so a probe added there without a line here is caught too.
PROBES=(
  WL_KBPTR_KEY_CHANNEL # popup-safe mode, 0002
  --drag               # drag ACTION, 0003/0004
  peek_alpha           # peek on SPACE, 0005
  --hold               # hold ACTION, 0006
  double_click_ms      # double click by pressing again, 0007
  --modifiers          # a click with Ctrl, Alt, Shift or Super held
  --modifiers-file     # ...toggled while the overlay is up, no relaunch
)
for probe in "${PROBES[@]}"; do
  if grep -qa -- "$probe" "$BIN"; then ok "probe finds '$probe'"; else
    no "probe finds '$probe' -- imthemousenow will think this build lacks it"
  fi
done

# --- 3. options imthemousenow passes ------------------------------------------------
# The shapes bin/imthemousenow builds, with values of the kind it builds them
# from: colours are a theme's #rrggbb with an alpha on the end.
accepts "general.peek_alpha" -o general.peek_alpha=0.1
accepts "general.peek_alpha at 1 (off)" -o general.peek_alpha=1
accepts "mode_click.double_click_ms" -o mode_click.double_click_ms=400
accepts "mode_click.double_click_radius" -o mode_click.double_click_radius=12
accepts "mode_click.double_click_color" -o 'mode_click.double_click_color=#7aa2f7ee'
accepts "all three double-click options together" \
  -o mode_click.double_click_ms=400 -o mode_click.double_click_radius=12 \
  -o 'mode_click.double_click_color=#7aa2f7ee'
accepts "an ACTION's tint on every mode" \
  -o 'mode_tile.label_select_color=#f7768eff' \
  -o 'mode_tile.selectable_border_color=#f7768eaa' \
  -o 'mode_floating.label_select_color=#f7768eff' \
  -o 'mode_floating.selectable_border_color=#f7768eee' \
  -o 'mode_bisect.pointer_color=#f7768edd'
accepts "a label font on every mode" \
  -o 'mode_tile.label_font_family=JetBrainsMono Nerd Font' \
  -o 'mode_floating.label_font_family=JetBrainsMono Nerd Font' \
  -o 'mode_bisect.label_font_family=JetBrainsMono Nerd Font'
for modes in tile,bisect,click floating,click floating,bisect,click tile,split,click; do
  accepts "modes=$modes" -o "modes=$modes"
done

# The same keys in a config file, which is how the compiled config reaches it.
cat >"$WORK/config" <<'EOF'
[general]
peek_alpha=0.1

[mode_click]
double_click_ms=400
double_click_radius=12
double_click_color=#7aa2f7ee
EOF
accepts "the patched keys in a config file" -c "$WORK/config"

# --- 4. arguments ------------------------------------------------------------------
# --drag x1,y1,x2,y2,duration_ms and --hold x,y, as bin/imthemousenow and
# bin/imthemousenow-hold build them. -O as well: both need an output, and the
# wrapper always names one.
accepts "--drag x1,y1,x2,y2,ms" -O DP-9 --drag 100,200,900,700,250
accepts "--drag with a zero duration" -O DP-9 --drag 100,200,900,700,0
accepts "--drag to negative coordinates (another monitor)" -O DP-9 --drag 100,200,-900,-50,250
rejects "--drag missing its duration" -O DP-9 --drag 100,200,900,700
rejects "--drag with a negative duration" -O DP-9 --drag 100,200,900,700,-1
accepts "--hold x,y" -O DP-9 --hold 640,360
rejects "--hold with one coordinate" -O DP-9 --hold 640
# --modifiers, on every kind of press, and empty because the wrapper may pass
# an empty list rather than leave the flag out.
accepts "--modifiers on a click" -O DP-9 --modifiers ctrl,alt,shift,super -o modes=tile,click
accepts "--modifiers empty" -O DP-9 --modifiers ''
accepts "--modifiers on a drag" -O DP-9 --modifiers ctrl --drag 100,200,900,700,250
accepts "--modifiers on a hold" -O DP-9 --modifiers shift,super --hold 640,360
rejects "--modifiers with a name it does not know" -O DP-9 --modifiers ctrl,hyper
accepts "--modifiers with spaces, as the session writes them" -O DP-9 --modifiers 'ctrl alt'
accepts "--modifiers-file, even one that is not there yet" -O DP-9 --modifiers-file /nonexistent/modifiers

# --- 5. against a real imthemousenow ---------------------------------------------
# The same questions, asked by the wrapper itself: its probes see this build
# (first on PATH), and every command its --dry-run prints for every
# combination of the four axes has to be accepted.
if [[ ! -x $PLUGIN/bin/imthemousenow ]]; then
  printf 'skip  no imthemousenow checkout at %s; the checks above still ran\n' "$PLUGIN"
else
  PLUGIN="$(cd "$PLUGIN" && pwd)"
  printf -- '---   against %s (%s)\n' "$PLUGIN" \
    "$(git -C "$PLUGIN" rev-parse --short HEAD 2>/dev/null || echo 'not a git checkout')"

  # Every string the checkout probes for must be on the list in section 2.
  mapfile -t theirs < <(
    { grep -shoE 'binary_knows [A-Za-z_-]+' "$PLUGIN"/bin/* | awk '{print $2}'
      grep -h '^requires *= *"' "$PLUGIN/config.default.toml" | sed 's/.*= *"\(.*\)".*/\1/'
    } | sort -u)
  for probe in "${theirs[@]}"; do
    [[ " ${PROBES[*]} " == *" $probe "* ]] && ok "imthemousenow's probe '$probe' is tested here" ||
      no "imthemousenow probes for '$probe', which this test does not check -- add it to PROBES"
  done

  mkdir -p "$WORK/path"
  ln -s "$BIN" "$WORK/path/wl-kbptr"
  cat >"$WORK/path/hyprctl" <<'STUB'
#!/bin/bash
case "$*" in
  *monitors*) echo '[{"name":"DP-9","x":0,"y":0,"width":1920,"height":1080,"scale":1,"focused":true}]' ;;
  *activewindow*) echo '{"at":[400,250],"size":[800,600],"address":"0xabc"}' ;;
  *clients*) echo '[]' ;;
  *binds*) echo '[]' ;;
  *) echo '{}' ;;
esac
STUB
  chmod +x "$WORK/path/hyprctl"

  cfg() { env MOUSENOW_PLUGIN_DIR="$PLUGIN" "$PLUGIN/bin/imthemousenow-config" "$@"; }
  valid() { cfg env | sed -n "s/^MOUSENOW_CFG_VALID_${1^^}='\(.*\)'$/\1/p"; }

  # The compiled config is passed with -c, so it has to parse on its own too.
  compiled="$(cfg compile --force 2>/dev/null)"
  if [[ -f $compiled ]]; then accepts "imthemousenow's compiled config" -c "$compiled"
  else no "imthemousenow-config compile produced a config file"; fi

  runs=0 seen_peek=0 seen_double=0
  for mode in $(valid mode); do
    for scope in $(valid scope); do
      for action in $(valid action); do
        for lifetime in $(valid lifetime); do
          label="$mode / $scope / $action / $lifetime"
          line="$(env PATH="$WORK/path:$PATH" MOUSENOW_PLUGIN_DIR="$PLUGIN" \
            "$PLUGIN/bin/imthemousenow" --mode "$mode" --scope "$scope" \
            --action "$action" --lifetime "$lifetime" -n 2>"$WORK/dry.err" | grep '^wl-kbptr ')"
          if [[ -z $line ]]; then
            no "$label: --dry-run printed no command"; sed 's/^/        /' "$WORK/dry.err" | head -3
            continue
          fi
          line="${line%%   # regions on stdin}"
          # The wrapper printed it with %q, so it reads back as the same words.
          eval "cmd=(${line#wl-kbptr })"
          [[ $line == *general.peek_alpha* ]] && seen_peek=1
          [[ $line == *double_click_ms* ]] && seen_double=1
          accepts "$label" "${cmd[@]}"
          runs=$((runs + 1))
        done
      done
    done
  done
  ((runs > 0)) || no "at least one combination was tried"

  # If the probes had failed, the wrapper would have left these options out and
  # every command above would still have parsed -- passing for the wrong reason.
  ((seen_peek)) && ok "the wrapper's peek probe found this build, and passed peek_alpha" ||
    no "the wrapper never passed general.peek_alpha -- its probe did not find this build"
  ((seen_double)) && ok "the wrapper's double-click probe found this build, and armed it" ||
    no "the wrapper never passed double_click_ms -- its probe did not find this build (or gsettings says 0)"
fi

echo
if ((fails)); then echo "$fails failed"; exit 1; fi
echo "all passed"
