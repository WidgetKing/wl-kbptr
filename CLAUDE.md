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

Moving to a newer upstream is the same procedure: a `work/` branch rebased onto
the new upstream commit, tests, live check, then merge, and move the
`pin/<commit>` tag.

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
