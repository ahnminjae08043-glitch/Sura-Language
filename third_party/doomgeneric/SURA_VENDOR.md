# Vendored: doomgeneric

- Upstream: https://github.com/ozkl/doomgeneric
- Commit: dcb7a8d ("boolean fix")
- License: GPL-2.0 (see LICENSE)
- Vendored: 2026-08-31, upstream `.git` metadata removed for flat vendoring.

Local changes, if any, live outside this directory in `os/doom/`.

Note: upstream `.gitignore` contains a `doomgeneric` pattern meant for the
built executable, which also matches the `doomgeneric/` source directory on a
fresh vendoring. The sources were therefore force-added (`git add -f`); new
files added under that directory later must be force-added too.
