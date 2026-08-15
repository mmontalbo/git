# Foundation: tags, primitives, and the rules between them

git-organize moves a source tree toward a declared layout. This note
states the two ideas the tool rests on, so that every rule and every
action grounds in them: files and directories carry tags, and the
machine has a small closed set of mechanical operations. A rule states
a relation between tags; enforcement is the machine converging the tree
until the relation holds.

This sits under FRAMING.md (the git-primitive spine and the Terraform
resemblance) and DESIGN.md (the concrete git adapter).

## 1. The substrate: tags

A tag is a label on a file or a directory. git already supplies the
mechanism: .gitattributes is an open key=value namespace and
`git check-attr` reads it. The tool already uses one tag, `area=`; the
foundation generalizes from that one tag to a namespace of them.

A tagger is the logic that assigns a tag. Two kinds:

- Declared: a tag written down, for example `.gitattributes` with
  `/commit-graph.c area=pack`. The maintainer states it.
- Derived: a tag computed from evidence, for example the modal
  commit-subject prefix (history), or the include-graph cluster
  (structure). The maintainer supplies the computation.

Taggers have precedence, the way git attributes and config resolve to
a last-match win. A declared tag outranks a derived one. Reading keeps
every tagger's opinion, which is needed to compare them; placement uses
the precedence-resolved effective tag.

The namespace is open and maintainer-defined. The git adapter uses:

- area: which subsystem a file belongs to (the placement tag).
- role: public or internal, the carve convention, now an explicit tag
  rather than logic hidden in the enforcer.
- kind: a single library object, or a program (feasibility).

Directories carry tags too. The line `odb: object odb oid ...` in the
layout reads as "the directory odb/ is tagged to own the area values
{object, odb, oid, ...}." A directory can also carry a constraint tag,
for example public-root (interface headers stay here) or frozen (do not
add to this directory).

## 2. Rules are relations between tags

With tags as the substrate, every rule is a statement about tags.

- Placement. A file belongs in the directory whose tags match its
  effective area tag. This is one uniform match, files to directories,
  and it replaces the special-case map.
- Adherence reduces to two questions the core answers from tags, plus
  conflicts a plugin may raise:
  - Is the file in its declared area? If not, it is a would-be rename.
  - Do the taggers of its area agree? If they disagree, the rename is
    a placement conflict.
  Beyond those, a would-be rename can be blocked by a conflict a domain
  plugin flags: enforcing it would violate a domain invariant (a
  requirement conflict, for example moving a program breaks the build),
  or the operation cannot be performed (a mechanical conflict). The
  core owns only the placement conflict; every other reason is supplied
  by a plugin, and the core never speculates about domain feasibility.

What tags do not express is how to make a move safe. Which references
to rewrite, and whether the artifact still holds after a move, is the
reference model and the validator, not tag logic. Tags say what should
be true and which files violate it; the primitives and the validator
make it true.

## 3. The mechanical primitives: the capability contract

The machine can do a small closed set of things. Enumerating them up
front bounds what any rule can ask for.

Mutations (the closed set):

1. Rename a file. Content-preserving, and it must be an R100 rename;
   git's rename detection is the proof.
2. Rewrite a reference. One line of text that points at the moved file:
   a C `#include`, a build-list entry, a per-object build rule. Which
   lines count as references is the reference-model plugin.
3. Create a container. Make a target directory.
4. Record. Stage, and optionally commit.

Checks (not mutations):

- The pure-rename gate: every moved file is R100. A git primitive,
  domain-agnostic.
- The validator: the artifact still holds. A domain plugin, the build
  for git's C sources, resolving links for a documentation tree.

The negative space is the point. The machine does not create content,
does not delete, and does not change meaning. That closed envelope is
why the operations are non-destructive, reversible, and order
independent, and why blame and `log --follow` survive.

Grounding rules in the primitives: a rule can only ever entail "rename
these files, and rewrite the references that bind to them." So a
declared desired layout is limited to the layouts reachable by pure
renames. A rule cannot ask the machine to merge two files, delete one,
or edit its meaning, because those are not primitives.

## 4. The layering

- Core, domain-agnostic: the four primitives, the pure-rename gate, and
  tag matching. No language, build system, or attribute string.
- Plugins, domain-specific: taggers (produce tags), the reference model
  (which lines are references), and the validator (the invariant).
- Rules: tag relations that select which primitive to apply where.

The validator is the only place domain knowledge decides go or no-go.
Everything mechanical is the closed primitive set.

## 5. The levers: rectifying a violation

Reconciliation is bidirectional: converge the file to the rule, or
amend the rule to the file.

- Converge the file: apply the rename and its reference rewrites, gated
  by the pure-rename check and the validator.
- Amend the rule: record a declared tag that outranks the derived one.
  A resolution recorded this way reapplies on the next run, which is
  the reuse-recorded-resolution behavior without a cryptic name.
- Restructure: for a file the kind tag pins, a program, the only way to
  make it movable is a domain change, for example splitting the build.
  This is not mechanical, so the tool only reports it.

## 6. Decisions taken (revisable)

1. Directories are tagged; the layout is directory tags, and placement
   is one file-tag-to-directory-tag match. This folds the map and the
   public-root convention into the same mechanism.
2. Tags are an open namespace (area, role, kind, and more), not one
   hardcoded tag.
3. Tags are multi-valued at read time, keeping every tagger's opinion;
   precedence resolves the effective placement tag, and agreement
   compares the rest.
4. There is one blocked state, conflict, carrying a reason. The core
   computes the placement reason from tags; domain plugins flag their
   own reasons (requirement, mechanical). The former held or pinned
   state is now a conflict with a requirement reason.

## 7. The vocabulary, derived

The terms fall out of the foundation. Each is grounded below, with the
older word it replaces.

Substrate:

- tag: a label on a file or directory, a git attribute.
- tagger: logic that assigns a tag (was Signal), declared or derived.
- effective tag: the precedence-resolved value used for placement.
- area, role, kind: the git adapter's tags. area is the placement tag
  (was the sole "area"); role is public or internal (was the hidden
  carve convention); kind is lib-object or program (feasibility).
- override: a declared tag at highest precedence (unchanged).
- layout: the file of declared directory tags and placement rules (was
  the unnamed map file, referenced in code only as mapref).

Per-file state. This is git status: one state model, rendered long
(phrases), short (XY codes), and porcelain (stable for scripts). The
expected tree is the declared layout, and the only change-type organize
emits on a source is renamed (see section 8).

- clean: the file is in its declared area, nothing to do (git:
  unmodified, "working tree clean"; the tool already prints "layout is
  clean"). Was placed or ok.
- renamed: the rules would relocate it, and apply stages it as a
  rename (git: renamed, short code R). This is the one change-type
  organize produces, shown with its target, "-> odb/". Was ready.
- conflict: the rule cannot be auto-applied to a would-be rename (git:
  unmerged, its own status section, short code U); apply --unconflicted
  skips every conflict. A conflict carries a reason:
  - placement: the taggers of its area disagree (core-computed).
    Resolve by deciding or overriding the area. Was contested.
  - requirement: enforcing the move would violate a domain invariant,
    for example moving a program breaks the build (a domain plugin
    flags it). Resolve by restructuring, or leave. Was held or marked;
    not skip-worktree, which names a different git bit.
  - mechanical: the operation cannot be performed, for example a name
    collision or a reference the adapter cannot rewrite. Resolve by
    clearing the obstruction.
  Reasons map to git's unmerged sub-types (both modified, added by us,
  deleted by them), which also resolve differently.
- untracked: no rule addresses the file, a gap a new rule could close
  (git: untracked, ??; at the attribute level its area is unspecified).
- ignored: a rule deliberately excludes the file (git: ignored, !!).

Agreement, the merge outcome of a renamed file:

- agreed: the two taggers give the same area (was corroborated).
- declared-only: only the effective tagger spoke, none corroborated it
  (was unverified, which implied a failed check).

Commands, each grounded in a primitive:

- check: read-only report, mutates nothing (git: fsck, verify).
- status: the drift view (git status), with --by-area (git: dirstat)
  and a versioned --porcelain machine format.
- apply: run the primitives; --unconflicted for the safe subset (agreed
  plus declared-only), --area to scope one directory.
- todo: the worklist of conflicts needing a decision, each with its
  reason and the tag edit that records the decision (was markers, which
  named git's in-file conflict text).
- resolve: settle a conflict, either --declared or --signal, or edit
  the tag. The flags name the two sides directly rather than reusing
  git's ours and theirs, which its own docs warn can appear swapped.

Architecture:

- Tagger, Policy, Enforcer: the three seams. Signal becomes Tagger
  under this model. Policy is the placement rule over tags. Enforcer
  owns the primitives and the validator. (Provider, from Terraform, is
  the alternative to Enforcer if the tool leans further that way.)
- adapter: the domain bundle (was triple, which counted arity and
  taught nothing). git-c is the reference adapter.

New concepts the foundation names:

- target: the physical directory an area maps to. area is logical; the
  target is where it lands. One area can map to more than one target
  (C++ include and src), which the current single-target assumption
  cannot yet express.
- the pure-rename gate and the validator: the two named checks, kept
  distinct so "build" is never mistaken for the gate.
- the plan: the reviewable set of moves before apply, the artifact a
  maintainer approves.

## 8. Rendered as git status

organize has git's three-tree shape: the current tree is the baseline,
the declared layout is the expected tree, and organize status reports
the divergence between the two. git already names every divergence, so
organize status is git status with the expected tree set to the
declared layout and the only change-type set to renamed.

git has seven change types: new file, copied, deleted, modified,
renamed, typechange, unmerged (wt-status.c:308-322). organize emits
exactly one on a source: renamed. It never creates, deletes, modifies,
copies, or typechanges a source. That is the safety envelope of section
3 stated in git's own vocabulary: organize status is a git status that
can only ever say renamed. The incidental modified on Makefile and
meson.build is the reference-rewrite side effect of a move, not a
placement state.

All five states are direct git reuse: clean, renamed, unmerged
(conflict), untracked, ignored. A conflict's reason maps to git's
unmerged sub-types, which resolve differently (both modified, added by
us, deleted by them). git has no sub-type for "the move would break the
build", so the requirement and mechanical reasons are organize's
extension of that sub-type idea; the mechanism is git's, the reasons
are the domain's.

The workflow maps the same way, status then add then commit:

- organize status is git status: the divergence as pending renames,
  plus the unmerged, untracked, and ignored sections.
- organize apply is git mv: it stages the renames, which then read as
  "Changes to be committed: renamed: old -> new".
- commit records them.

So a file the rules would relocate is a pending rename; apply stages
it; commit records it.
