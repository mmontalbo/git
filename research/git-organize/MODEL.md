# Model: interpret attributes, transform the tree

This supersedes the Signal/Policy/Enforcer seams. The tool is a
reconciler over per-file desires: a project interprets attributes into
what should happen to each file, and the core reconciles that against
the tree and executes a closed set of relocation transformations. The
core owns no organization vocabulary; a git project supplies it.

## The three roles

- Interpreter (project): assigns attributes and reads them into a
  per-file Desire. Owns the vocabulary (git: organize.subsystem,
  organize.role, organize.pin) and what those attributes mean.
- Transformer (project): executes a move safely and says which moves
  are feasible. Owns scope, the reference model, and the validator.
- Core (generic): reconciles Desires against the tree into the four
  states, builds the plan, runs the transformer, renders status/apply.
  Attribute-blind and path-blind.

An organization attribute (subsystem, section, module, package) is
always project vocabulary, so there is no generic organization
attribute. The generic layer is the attribute mechanism plus this
engine.

## Data the core knows

Desire, per file, from the Interpreter:

    Desire(place: Dir | None, hold: str | None)

- place is where the file belongs; None means unmanaged (no rule).
- hold, when set, is a project reason the file must stay put despite
  place (a declared block, for example organize.pin). None means free
  to move.

The core asks the Transformer for the transformations that would
organize each file (a move into place, a reference patch, or both),
then reconciles:

- no desire, nothing to patch            -> untracked
- already in place, references resolve   -> organized
- a transformation applies cleanly       -> unorganized (in the plan)
- a transformation is wanted but blocked  -> conflict

unorganized is any file the plan transforms to reach organized: usually
a rename into place, sometimes only a reference patch (a referrer whose
link to a moved file is stale), sometimes both. The core counts
organized vs unorganized vs untracked and never names the transformation
kind; the project surfaces it (a rename, a patch) and any reasoning. A
conflict is an unorganized file the tool cannot auto-apply, carrying the
project's reason: a hold attribute or the transformer's block. apply
performs the unorganized plan and holds the conflicts. The plan is the
source of truth for what is unorganized. One interpreter suffices: the
reconciler reads a single desired placement per file and converges the
tree to it, so the core carries no cross-check between signals.

## Interpreter interface

    desires(scope) -> {file: Desire}
        Read the effective attributes for each file and return its
        Desire. One interpreter drives placement. A project that wants
        extra organizing heuristics expresses them as its own organize.*
        attributes that this interpreter reads into the Desire (a hold,
        or a different place). The core never grows a second seam for
        them.

    resolution(f, place) -> [str]
        The human instruction that records "f belongs in place": for
        git, the attribute edit. The core prints these lines under a
        conflict; it never composes an attribute name itself.

Two flavors:

- Config default (no code): the config names one attribute, e.g.
  organize.subsystem. desires reads git check-attr for it: place is the
  value (a dir), hold is set from organize.pin if present, place is None
  when the attribute is Unspecified. resolution says "set
  organize.subsystem=&lt;place&gt; on f". This is the zero-code path for a
  flat domain.
- Plugin (code): full control. The git interpreter computes subsystem
  (the modal commit-subject prefix, or a declared organize.subsystem),
  role (include analysis: a public header gets place=root and a hold, an
  internal header rides its source's place), and pin (build membership,
  a program gets a hold). The pairing (an internal header follows its
  source) lives here, so the core never learns about headers.

Attributes are the interface between human and interpreter: a person
overrides by setting organize.subsystem on a path, and git's precedence
(a more specific line wins, info/attributes on top) resolves declared
against derived for free.

## Transformer interface

    scope() -> {file}
        The files in play (git: tracked root sources).

    already_at(f, dir) -> bool
        Whether f already sits in dir. Keeps the core path-blind.

    plan(moves) -> Plan
        (file, dir) pairs the core wants performed. Returns the feasible
        moves, the blocked ones as (file, reason), and the reference
        edits (which lines name a moved file and how they repoint). The
        blocked reasons become mechanical conflicts.

    apply(plan, commit) -> Result
        Run the closed primitive set and gate: rename (must be R100),
        rewrite references, mkdir the target, record; run the validator;
        roll back every rename and edit on any failure. Nothing creates
        content, deletes, or changes meaning.

## Core

The core holds only: the reconcile rules above, the states organized /
unorganized / conflict / untracked, the plan/status/apply loop, and the
R100 gate (a git primitive, domain-agnostic). The transformation
alphabet is closed (rename, patch a reference, mkdir, record) and the
core is transformation-agnostic: it counts what the plan touches and
lets the project name the kind. It names no attribute, no directory
arithmetic, no language. Commands stay status (the plan and the drift)
and apply (converge), with --by-area, --conflicts, --exit-code as views
of the same counts.

## Apply: safe operations first

apply emits a sequence of git operations and runs only the provably safe
subset, holding the rest. The safe subset is the git verbs that always
apply and preserve content: an R100 rename to a free target (a clean
tracked file, a destination that does not exist, the blob unchanged
because a moved file is never edited), and a reference edit only when it
is unambiguous (a build-list line with one match). git rm and any
content-creating add stay out, so the output tree keeps the input's blob
OIDs, only at new paths, except the reference files patched in place. The
result is an ordinary staged commit, reviewed with git diff and reverted
with git reset; the renames are R100, so history follows.

The unit of safety is a file's whole transformation, the rename plus
every reference edit it entails, not the rename alone. A file whose
entailed edit is not provably safe is held whole, so apply never leaves a
half-moved, unbuildable tree. conflict therefore starts as "not provably
in the safe subset"; the finer split (a git-mechanical reject a human
resolves, versus a policy that forbids the move) is a later refinement.

A requirement block is such a policy: a program cannot relocate because
its name derives from its path. Once that block is a policy attribute (a
program desires place = root), its command sequence is empty and it reads
clean, not conflict, so the separate requirement-conflict category
collapses into the policy. The generic core keeps only the mechanical
notion of a conflict; the domain expresses its constraints as attributes.

## git's instance

Attributes (namespaced; git attribute names allow [-A-Za-z0-9_.], so a
config-style namespace is legal, attr.c:199):

- organize.subsystem = &lt;dir&gt;   where a file belongs (odb, refs, pack)
- organize.role = public|internal  a header kept at root vs moving
- organize.pin                     cannot relocate (a program)

Interpreter: the git plugin. subsystem from the modal commit-subject
prefix or a declared organize.subsystem; role from the include graph;
pin from Makefile membership; internal headers ride their source.
Include cohesion is an advisory lens a maintainer runs as a separate
view while authoring the attributes, not a core cross-check.

Transformer: git mv, repoint the Makefile and meson build lists, run
make as the validator, reset on failure.

The declared layout is then an attribute set a maintainer reads with
git check-attr organize.subsystem -- &lt;file&gt;, and the mailing-list
artifact is that attribute set reproducing the subsystem carve.

## What this supersedes

- Signal + Policy collapse into the Interpreter (assigning and
  interpreting attributes share one vocabulary, so they are one role).
- Enforcer becomes the Transformer.
- git-layout.map and the area= override and the second signal collapse
  into attributes plus git's precedence; the token to dir table moves
  inside the git interpreter.
- is_source, paired_internal_header, and kept_public leave the core:
  role and pin and pairing are the interpreter's, so the core stops
  carrying C-carve concepts.

## Migration

The current code is Signal/Policy/Enforcer. This is the target. The
smallest first slice: rename the Enforcer to Transformer with a
plan/apply/already_at surface, fold Signal+Policy into an Interpreter
returning Desire, read organize.subsystem via check-attr in the git
interpreter, and keep the core's plan-derived counts (already landed).
FOUNDATION.md and EXTENDING.md fold into this doc; FRAMING.md and
DESIGN.md are the prior history.
