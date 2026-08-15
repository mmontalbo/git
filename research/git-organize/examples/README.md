# organize examples: config as a domain, and git's declarative subset

Two configs for `config_adapter.py`, the domain-neutral adapter that
fills the Signal, Policy, and Enforcer seams from a JSON config plus
command call-outs. Neither the adapter nor the native engine
(`organize_core.py`) names a build system, a language, or a link
syntax; every such string is in a config here.

Run either from `research/git-organize`:

    python3 git-organize --config examples/docs/docs.organize.json status
    python3 git-organize --config examples/git.organize.json    status

## docs/ : a config-only domain, zero Python

A toy documentation tree proving the tag/check/apply API on a second
domain with no adapter code at all. The config wires:

- `map` (`layout.map`): `guides: guide` and `reference: reference`, the
  same "directory: token token" format git's layout map uses.
- `members` (`["**/*.md"]`): the scope glob.
- `tagCmd` (`tag.sh`): the tag protocol, reading each page's `area:`
  frontmatter and emitting `path<TAB>area=NAME`.
- `references` (`["[{title}]({path})"]`): the reference model, so a
  moved page's inbound `[text](path)` links repoint to the new relative
  path.
- `validate` (`validate.sh`): the authoritative check, a link resolver
  that exits nonzero on a broken link.

`apply --unconflicted` moves every page into its area directory, repoints
the cross-links, and gates on `validate.sh`. Re-running reports the
layout clean. No `relocateCmd` (every page moves) and no `overridesCmd`,
so the whole domain is five config lines and two short shell scripts.

## git.organize.json : git's declarative subset

The same `config_adapter.py`, pointed at git's C tree, to show the
separation. It wires:

- `map`: `../git-layout.map`, git's own area map.
- `members`: `["*.c", "*.h"]`, the flat root sources.
- `references`: `["#include \"{path}\""]`, the C include reference model.
- `validate`: `make`, the build.
- `overridesCmd` (`git-overrides.sh`): reads the per-path `area=`
  attributes from `.gitattributes` through `git check-attr`, the
  declared, highest-precedence label.

Run `status` and this config labels roughly 300 root sources across all
14 areas from the map tokens in their names plus the gitattributes
overrides. This is git's simple half: the map, the membership, the
include reference syntax, the build validator, and the declared
overrides are all data, not code.

## What stays in the git-c plugin, and why

The config subset above is deliberately git's *simple* choices. The
git-c adapter (`organize_git.py`) stays a Python plugin because its logic
is irreducible, not expressible as a config value or a stdin/stdout
call-out:

- **Include-cohesion clustering.** The placement cross-check runs an
  agglomerative clustering over the include graph (`organize_cohesion.py`
  plus `research/lib-reorg`) to corroborate or contest the history
  label. A config value cannot express a clustering algorithm.
- **The internal-header fixpoint.** A same-stem `.h` co-moves with its
  `.c` only when every file that includes it also moves; a public
  interface header stays at the root. Deciding internal vs public is a
  fixpoint over the include graph, not a glob.
- **LIB_OBJS relocatability.** A source may move only when the Makefile
  builds it as exactly one library object; a program cannot move because
  its name derives from its path. Reading that from the build is domain
  logic; the generic adapter offers a `relocateCmd` seam for the simple
  cases, but git's answer needs the Makefile parse.
- **Per-object rule reparenting.** Moving a source rewrites its
  `NAME.sp NAME.s NAME.o:` triplet and its `LIB_OBJS`/`meson.build`
  lines, then re-checks that no root object rule survives. That is a
  multi-file, order-sensitive edit with a safety re-scan, past what a
  reference-pattern swap can do.

The point of the split: `config_adapter.py` holds zero domain literals
(`grep -nE 'make|LIB_OBJS|#include|\.c\b|\.md\b|meson' config_adapter.py`
returns nothing), and `organize_core.py` names no build system, language,
or attribute file that drives behavior (its git words are status UX prose
and comments; see its purity docstring). Git's simple choices are config
here; git's rich logic is the labeled plugin. A new domain like docs/
reaches the config tier and needs no Python.

## A known core coupling (reported, not patched)

The core's default `status` summary and its `apply --unconflicted` route
through `_agree_verdicts` (`organize_core.py:424`, `:472`), which is
hardwired to the `git-c` and `cohesion` triples for the
history-vs-includes agreement cross-check, a git specific. A domain with
neither triple cannot use that path. Rather than edit the core, the
launcher's `--config` branch dispatches those two commands through the
generic seams (`_place_all`, `_files_for`, and the Enforcer) in
`config_adapter.dispatch`; `status <area>` and `apply --area X` already
avoid `_agree_verdicts`, so they defer to the core unchanged. Generalizing
the cross-check to any pair of signals would let the config path use the
core summary directly.
