# This fork

This is not the wl-kbptr project. It is WidgetKing's fork of
[moverest/wl-kbptr](https://github.com/moverest/wl-kbptr), and the program is
moverest's. It exists for one reason: to carry the changes
[imthemousenow](https://github.com/WidgetKing/imthemousenow) (checked out at
`../imthemousenow`) needs from it -- popup-safe mode, drag, hold, peek, double
click, one crash fix.

## Branches

- `imthemousenow` -- **what gets installed.** imthemousenow's `install.sh`
  builds the tip of this branch, on every machine, at the next install or
  update. Upstream's history up to tag `pin/<commit>`, imthemousenow's commits
  on top.
- `main` -- a mirror of upstream (`upstream/main`). Never commit to it.
- `work/<name>` -- where every change is made. See below.
- `drill/rebase-onto-main` -- a rehearsal of moving the pin, kept for the
  conflict resolutions git has recorded (`rerere`). Not pushed.

Remotes: `origin` is the fork, `upstream` is moverest's repo and is only ever
fetched from. Nothing here is sent upstream -- no pull requests, no issues --
by choice.

## Never experiment on `imthemousenow`

A commit pushed to `imthemousenow` is what the next install everywhere builds.
So no change is made on it directly, however small. Every change goes:

1. **Branch off it.** `git switch -c work/<name> imthemousenow`
2. **Make the change there**, as one commit per change, with a message that
   says what it does and why.
3. **Run the contract tests:** `imthemousenow/test.sh`. They build this tree
   and check it against what imthemousenow sends it; they need no compositor.
4. **Push the work branch**, not `imthemousenow`:
   `git push -u origin work/<name>`
5. **Try it live.** In `../imthemousenow`, Tristan runs
   `./install.sh --dev --branch work/<name>` in a real terminal (it needs sudo
   for pacman, which a tool call cannot give it). Then check the change on the
   running compositor. Tests passing is not "working"; seeing it work is.
6. **Only once it is confirmed working, merge it:**
   ```
   git switch imthemousenow
   git rebase imthemousenow work/<name>     # if imthemousenow has moved on
   git switch imthemousenow
   git merge --ff-only work/<name>
   imthemousenow/test.sh
   git push origin imthemousenow
   git push origin --delete work/<name>
   ```
   Fast-forward only, so the branch stays one commit per change with no merge
   commits. Then a plain `./install.sh --dev` in imthemousenow installs it.

To abandon a work branch, a plain `./install.sh --dev` puts the real branch
back; delete the work branch when done with it.

Moving to a newer upstream uses the same procedure; the next section says how
to judge whether it is safe.

## Bringing in upstream changes

Nothing reaches this fork from upstream on its own. The pin (`pin/<commit>`)
moves only when a change upstream is worth having -- a fix, a build fix for a
new library version, a feature -- and never just because upstream moved.

**1. See what changed.**
```
git fetch upstream
git log --oneline imthemousenow..upstream/main --not pin/<commit>
git diff --stat pin/<commit> upstream/main
```
Then compare the files upstream touched with the map below.

**2. What our commits touch** (one row per commit, oldest first):

| Commit | Files | What it does |
|---|---|---|
| Destroy the pending frame callback | `main.c` | Crash fix at exit |
| Take keys from a channel | `main.c` | Popup-safe mode: keys read from a file named by `WL_KBPTR_KEY_CHANNEL`, no keyboard grab |
| Walk a path with a button held | `main.c`, `utils_wayland.*` | `--drag x1,y1,x2,y2,ms` |
| Say a drag path in layout coordinates | `main.c`, `utils_wayland.*` | Drag across monitors |
| Dim the overlay while a key is held | `config.*`, `main.c`, `mode*.c/h`, `state.h` | Peek: `general.peek_alpha`, key release on the channel |
| Hold a button down and be steered | `main.c`, `utils_wayland.*` | `--hold x,y` |
| Click twice when the committing key is pressed again | `config.*`, `main.c`, `state.h` | `mode_click.double_click_ms` (it also added `_radius`/`_color`, since removed) |
| Say where each click went | `main.c` | `WL_KBPTR_CLICK_REPORT=<path>`: each click writes `<output> <x> <y> <ms>` there |
| Draw nothing while the double-click window is open | `config.*`, `main.c` | The ring and its two options are gone; imthemousenow marks the click itself |

Upstream changes to files not in that table (`meson.build`, the protocols,
`config.example`, other modes) almost never conflict. Changes to `main.c` and
`config.c` usually will, and the conflicts are the thing to read carefully.

**3. What has happened before.** A rehearsal onto upstream `04d7ebd` (branch
`drill/rebase-onto-main`, not pushed) conflicted twice, both in `config.c`:
upstream gave every config field macro a description string, and our peek and
double-click fields were in the old form. Both were mechanical: keep
upstream's form, add our fields in it with a description. `rerere` is on and
recorded both resolutions, so a real rebase onto that point replays them. Look
at that branch before resolving by hand.

**4. What can break with no conflict at all.** Conflicts are the easy case.
The dangerous upstream change applies cleanly and changes something
imthemousenow relies on. Read the upstream diff for:

- **Option or section names** imthemousenow passes (`modes`, `mode_tile.*`,
  `mode_floating.*`, `mode_bisect.*`, `mode_click.button`, the colour and font
  keys). wl-kbptr rejects its whole config over one unknown option, so a
  rename upstream makes every chord silently stop working, not just one.
- **The mode chain and the click stage.** imthemousenow builds
  `modes=<a>,<b>,click` and relies on `click` being the last stage. Upstream
  has discussed moving click (and drag) out of the mode chain into flags
  (moverest/wl-kbptr PR #96). If that lands, imthemousenow has to change as
  well as this fork, and the drag, hold and double-click commits will need
  rethinking, not just rebasing.
- **Exit codes.** imthemousenow sets `general.cancellation_status_code=1` and
  reads the exit status to tell a finished selection from a cancelled one;
  continuous lifetime depends on it.
- **How keys and the surface are handled** in `main.c`: the key channel and
  the peek both sit in the keyboard and frame paths.

If upstream needs imthemousenow to change too, that is a change in
`../imthemousenow` as well, done alongside this one and landing at the same
time. The contract tests will show which side broke.

**5. Build and dependency changes** (a new OpenCV or other library major
version, meson changes): these usually are the reason to move the pin. Build
the work branch the way the package is built -- `imthemousenow/test.sh` does,
with OpenCV on -- and check `wl-kbptr --version` still says `opencv`.

**6. Decide, and say so plainly.** The result for Tristan is: what upstream
changed, in terms of behaviour; whether any of it touches what is in the map
or in point 4; whether the rebase conflicted and how each conflict was
resolved; and whether the contract tests pass. Then comes the usual live check
before anything is merged. If the answer is "not safe yet" or "needs a change
in imthemousenow", say that rather than forcing the rebase through.

After merging, move the tag: `git tag pin/<new> <new-upstream-commit>`, push
it, and update the rows above if a commit's files changed.

## Adding a feature imthemousenow uses

wl-kbptr rejects its whole config over one option it does not recognise, so
imthemousenow asks the installed binary for a string before it uses any
feature from here. A new feature therefore needs three things, not one:

- the change here,
- a `has_*()` probe (or an ACTION `requires`) in imthemousenow that looks for
  a string only this build contains,
- that string in `PROBES` in `imthemousenow/test.sh`, plus `accepts` lines for
  the options and arguments imthemousenow will send. The test fails if
  imthemousenow probes for something missing from `PROBES`.

## Commits

Authored as Tristan Ward with his GitHub noreply address (already set in this
repo's git config), with `Co-Authored-By: Claude ...` on every commit. Tristan
does not read C: explain a change in terms of what it does, not how the C does
it.

## Files that are not wl-kbptr's

`README.md` (the note at the top), `CLAUDE.md`, and everything under
`imthemousenow/`. Everything else is moverest's code, changed only by the
commits on `imthemousenow`.
