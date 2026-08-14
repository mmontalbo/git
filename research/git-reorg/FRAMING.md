# Framing: git primitives, domain plugins, and the Terraform resemblance

This tool reorganizes a source tree toward a declared layout. It is
built to rest on git's own primitives and to push everything
domain-specific out to plugins, so the same engine can reorganize C
sources, documentation, or any tree of line-based text.

## The primitive spine (core, domain-agnostic)

Git is domain-agnostic because it deals in trees, blobs, line-based
text, rename detection, and merge, and never in "does it build." The
tool rests on the same primitives:

- Tree plus rename detection. A reorg is a transformation of git's
  tree. The primitive operation is a content-preserving relocation, and
  git's own rename detection proves it: a move is well-formed at the
  tree level exactly when it is an R100 rename. "Is this a pure rename?"
  is answerable by git, with no domain knowledge.
- Line-based text plus diff. A reference to a file, a C `#include`, a
  Markdown `[link](path)`, a LaTeX `\input`, is a line of text, and
  rewriting it when the target moves is a diff hunk. The mechanism is
  core; the kind of reference is a plugin.
- Merge. The decision to move a file or to flag it is a three-way
  merge: base is the current layout, "ours" is the declared rule,
  "theirs" is the file's own structural evidence. Agreement applies
  cleanly; disagreement is a conflict marker.

## The plugin seams (where a domain enters)

A project supplies these; the tool ships a code default (the
"primitive use case": line-based text, the way Linux development is).

- Reference model: what counts as a reference and how it resolves.
  Code: `#include`, resolved by `-I` and same-directory search. Docs:
  `[link](path)`, resolves if the target exists.
- Scope and membership: which files are in play. Code: the Makefile's
  LIB_OBJS. Docs: every `*.md`.
- Signals: what evidence groups files. Code: commit-subject labels and
  include cohesion. Docs: frontmatter category and cross-link density.
- Validator: does the reorg preserve the artifact's invariant. Code:
  build plus test. Docs: links resolve, or nothing. This is where
  "build" lives; it is a plugin, not the gate.

## The gate, restated

An automatic apply gates on, in order:

1. Not contested. The rule and the evidence do not disagree (merge).
   Domain-agnostic; the signals are plugins.
2. Pure rename. Every moved file is R100 (git's rename detection).
   Domain-agnostic; the git-primitive floor. This is where a naive
   design would wrongly put "build."
3. References resolve. Guaranteed by the plan: it only emits moves that
   keep interfaces in place, so references still bind. The reference
   model is the plugin.
4. Validator. The domain plugin confirms the invariant. For git's C
   sources this is the build; it runs as the code provider, on top of
   the primitive gate, not as the gate itself.

## The Terraform resemblance, and where it diverges

The workflow shape is Terraform's, and the vocabulary is worth
borrowing for recognition: a declared desired state in line-based text
(the map, like a `.tf`), a plan that diffs desired against current
(`status`, like `terraform plan`), an apply that converges
(`apply --auto`, like `terraform apply`), pluggable providers (the
seams above), drift detection, idempotence, and a targeted subset
(`--area X`, like `-target`). Adopt these words.

Then name the divergences, because each is a git primitive replacing a
Terraform workaround, so people do not expect the wrong thing:

1. No state file. Terraform keeps `terraform.tfstate` because cloud
   reality is external and unversioned. Git's tree is the state:
   content-addressed, versioned, cheaply diffable; `git ls-files` and
   `git diff` are the refresh. We inherit none of the state pain (no
   drift-from-state, no locking, no corruption, no import).
2. The plan is a three-way merge, not a two-way command. Terraform
   trusts the `.tf` as ground truth and executes every diff. We treat
   the declared rules as fallible and corroborate them with independent
   evidence; where they conflict we do not apply, we flag. Terraform
   has no notion of "your declaration might be wrong."
3. Operations are pure renames: non-destructive, reversible, and order
   independent. Terraform does create, replace, and destroy, and needs
   a dependency graph to order them. Our only operation is an R100
   rename, so carves do not interfere and need no resource graph.
4. Reconciliation is bidirectional. Terraform only pushes file to
   reality. Our markers push reality back to the rules: the evidence
   proposes amending the declaration, and the resolution is recorded as
   a new rule.

## The capability beyond Terraform

Terraform's failure mode is apply or error. Ours is a structured
intervention request: the file, its declared home, the home its
evidence implies, and the disagreement, sized for a human or an LM to
resolve, with each resolution recorded back as a declarative rule. That
is a merge conflict with provenance, and it exists only because we
treat the declaration as fallible. Terraform converges a trusted plan;
we converge the trustworthy subset and escalate the rest with evidence.
