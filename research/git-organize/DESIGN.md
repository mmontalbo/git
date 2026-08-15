# organize: enforce a declared source layout

## Goal

Given a project that follows the `area:` commit-subject convention and
declares a map between areas and directories, move files that are out of
place and create the directories that need to exist, mechanically.

The framing is "enforce a declared layout," the way a formatter enforces a
declared style. The tool decides nothing about the layout. The map does, and
the project maintains the map.

## Inputs

- membership: which files are in scope (for git, the Makefile's LIB_OBJS).
- per-file area: the area a file belongs to. Two sources:
  - inferred from history: the modal `area:` prefix over the file's commits,
    weighted so small commits count more than large sweeps.
  - declared: an optional per-path `area=` attribute in `.gitattributes`,
    which overrides the inferred value for the cross-cutting files that
    history labels ambiguously.
- the map: area name to directory. A tracked file, modelled on `.mailmap`,
  its location set by a config key (for example `organize.mapFile`, default
  `.git-layout`). See `git-layout.map` here for the format.

## Subcommands

- `git organize check`: read-only. Report files whose area does not match their
  directory. Language-agnostic. This is the CI linter and the low-risk first
  product.
- `git organize plan`: dry run. Show the moves it would make.
- `git organize apply [--area X]`: perform one area's moves, with the cascade,
  then build and commit. One area at a time keeps each step reviewable.

## The cascade (the real work)

The move itself is `git mv`. The work is keeping the tree buildable after it:

- move a file's paired header with it.
- rewrite `#include "foo.h"` across the tree to the new path.
- update the build source lists that name the file by path (Makefile, meson).
- build, and stop if it fails.

This is language- and build-system-specific, so it lives in adapters, one per
(language, build system). git did this by hand when it created `odb/`.

## Distribution

Not a core builtin. git core moves and tracks bytes; it does not know that
`diff.c` belongs in `diff/`, and the cascade is C-and-build specific, which
core stays out of. The command ships as an external `organize` on PATH, so
`git organize` works, in `contrib/` if it earns it, the way `git-svn` and
`scalar` began.

## Dogfooding git

1. ship `git organize check` and a layout map for git's own tree.
2. run `check` in CI; it flags files whose commit label disagrees with their
   directory, and moves nothing.
3. use `apply --area odb`, one area at a time, for the actual carves.

This matches how the maintainers already work: carve deliberately, one
subsystem at a time. The linter flags; a human triggers each carve.

## Honest limits

- works only where the `area:` convention is followed consistently.
- the map is the irreducible human input; without a maintained map the tool is
  back to inventing structure, which does not generalise.
- renames and thin history make some files low-confidence; move only the
  confident ones and flag the rest.
- cross-cutting files (the utility layer) have no single area; the map needs an
  explicit rule for them.

## Status

Prototype on branch `mm/organize`. `organize check` is implemented here and
reuses the area computation from `research/lib-reorg/subsystems.py`. `plan`
and `apply`, and the cascade adapters, are not built yet.
