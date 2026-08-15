# Extending organize to another project

organize is built so the same engine that reorganizes git's C tree can
reorganize a documentation tree, a Rust crate, or any tree of line
based text. A project plugs in by answering three questions. The git-c
adapter is the first set of answers; this note is how to write another.
It sits alongside FOUNDATION.md (the tag substrate and the primitive
contract) and FRAMING.md.

## The three functions

A domain provides three operations. Everything else is generic.

1. tag(files) -> tags
   How should things be organized? Assign each file its tags: area
   (which group it belongs to), and optionally role (public or
   internal) and kind (movable or not). Directories carry tags too (the
   map: "this dir owns area X"). The desired layout is file tags matched
   to directory tags.

2. check(action, context) -> ok | reason
   Does an organize action work given the current tree, and if not,
   why? For a proposed move, return ok, or the reason it cannot proceed:
   - placement: the tags disagree on where it goes.
   - requirement: performing it would violate a domain invariant, for
     example a program's name derives from its path and the build would
     break.
   - mechanical: the operation cannot be performed, for example a name
     collision.
   check has a cheap half that predicts from context (it produces the
   renamed versus conflict split in status) and an authoritative half
   that runs after staging (the build, a link checker).

3. apply(action)
   How to perform the action. The primitives are a closed set: mv
   (rename), patch (repoint the references that name the moved file),
   mkdir (create a target directory), and record (stage, then commit).
   Of these only patch is domain specific; mv, mkdir, and record are
   generic. There is no create-content, delete, or modify-meaning, so
   every action is a non-destructive, reversible rename plus its
   reference repoints.

These power the two commands a user sees. status runs tag then check
(compute the desired layout, ask what can apply, split into renamed and
conflict). The apply command runs check then apply (perform the ones
that pass, gate on the authoritative validate).

## Filling each function: config, a command, or a plugin

A ladder from no code to full code. Fill each function the cheapest way
that expresses it:

- config: a value in the project's organize config (the map file, a
  glob of members, a reference pattern, a validate command line).
- command: an executable the tool calls out to, for logic config cannot
  express (a tagger, a relocatability check, a reference repointer).
- plugin: a Python module implementing the Signal, Policy, and Enforcer
  Protocols, for rich logic. The git-c adapter is one; writing a plugin
  keeps all the power the git cleanup used.

Most projects need config plus one or two commands. The build validator
alone (validate = "cargo build") already gets a domain a long way.

## The command protocol

Call-outs are batched and one-shot: the tool writes the whole file list
to the command's stdin once and reads its result, the way git's own
check-attr --stdin -z works. No process per file, and no long-running
protocol to implement.

tag command
  stdin:  NUL-separated file paths.
  stdout: lines "path<TAB>key=value", one per tag; a file may get
          several; no line means no tags.
  example: object.c<TAB>area=odb   and   odb.h<TAB>role=public
  The tool merges these with declared tags (overrides) by precedence. A
  second tag command may supply an independent signal for the agreement
  cross-check.

check, predictive
  stdin:  NUL-separated file paths.
  stdout: "path<TAB>ok" or "path<TAB>reason"; a path not named is
          movable.
  example: http-backend.c<TAB>built as a program, not a library object
  This produces the requirement conflicts, before anything moves.

check, authoritative (validate)
  A command line run in the tree after the moves are staged. Exit 0
  means the artifact still holds; nonzero rolls the batch back. This is
  the build for git, a link checker for docs, or nothing.

apply, patch (references)
  The tool does mv, mkdir, and record itself. The domain supplies how
  to repoint references to a moved file, either as patterns in config
  or as a command:
  - patterns: a list such as ['#include "{path}"', "'{path}'",
    "OBJS += {stem}.o"]. The tool finds lines that match and name a
    moved file and repoints the path.
  - patch command: stdin "oldpath<TAB>newpath" lines; the command edits
    the tree so references resolve, exit 0. Use this when the repoint is
    not a simple path swap, as with git's per-object build rules.

## Placement conflicts need no command

placement, the tags disagreeing on where a file goes, is computed by
the tool from the tags themselves, by comparing two taggers. It is not
a call-out. Supply a second tag command and the tool reports where the
two disagree, with the rule edit that records a decision.

## A docs tree, config only

  map:        guides: guide tutorial howto
  members:    **/*.md
  references: ['[{title}]({path})']
  validate:   linkcheck .

No tag command (a small one can read a frontmatter category if the map
tokens are not enough), no relocatability check (every page moves), no
patch command (the reference pattern is enough). Roughly five lines and
one validate command.

## git, the reference plugin

The git-c adapter answers the same three functions in Python because
its logic is rich:
- tag: commit-subject prefix and include cohesion produce area;
  membership and headers produce role and kind; overrides come from
  .gitattributes.
- check: relocatable iff the file is one library object; validate runs
  the build.
- apply: git mv, then repoint the Makefile and meson build lists and the
  per-object rules; public headers stay at root, so includes rarely
  need repointing.

## What is built, and what is next

Today the git-c adapter is a Python plugin, and the two call-out
patterns it uses (check-attr --stdin for tags, make for validate) are
the model for the command protocol above. The near-term work is a
config-driven adapter that fills tag, check, and apply from config and
commands, so a project like the docs tree runs with no Python, and
lifting the agreement cross-check off the hardcoded git-c and cohesion
names so it works for any domain.
