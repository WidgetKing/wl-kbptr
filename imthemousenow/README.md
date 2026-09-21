# imthemousenow/

Not part of wl-kbptr. This directory belongs to the `imthemousenow` branch of
this fork, and holds the tests that check the patched binary against what
[imthemousenow](https://github.com/WidgetKing/imthemousenow) sends it.

    imthemousenow/test.sh [IMTHEMOUSENOW_CHECKOUT]

It builds this tree into `imthemousenow/build/` and needs no compositor:
wl-kbptr reads and validates its whole configuration before it connects to
Wayland, so a command that gets as far as "Failed to connect to Wayland
compositor" has been accepted in full, and one that stops earlier has not.

With an imthemousenow checkout (by default `../imthemousenow`, next to this
one) it also runs that checkout's own `--dry-run` for every MODE, SCOPE,
ACTION and LIFETIME, with this build first on its `PATH`, and feeds each
command it prints back to this build. That is the real interface: the
wrapper's capability probes ask this binary, and the options they switch on
have to parse here.

imthemousenow's install builds the tip of this branch, so a push here is
what the next install or update anywhere builds. Run this first.
