"""organize config adapter: a domain-neutral Interpreter and Transformer.

One adapter that fills the two seams from a JSON config plus command
call-outs, so a project reorganizes with no Python. It holds no domain
literal: no build command, no language suffix, no attribute file name,
no reference syntax. Every such specific comes from the config file,
which names the map, the members glob, the tag command, the reference
patterns, and the validate command.

This is an adapter, not the pure core, so it runs subprocess, touches
the filesystem, compiles regexes, reads JSON, and expands globs. What
it never does is name a domain: read the config for that.

The config keys (JSON):
  name          the adapter (triple) name to register.
  map           path to a "dir: token token" layout map file.
  members       list of globs for the files in scope.
  tagCmd        optional command implementing the tag protocol; stdin
                is NUL-separated paths, stdout lines "path<TAB>key=value"
                with keys area, role, kind. Absent: a file's area is the
                map token that appears in its path.
  overridesCmd  optional command giving declared overrides, highest
                precedence; stdout lines "path<TAB>area=X".
  relocateCmd   optional predictive check; stdin paths, stdout
                "path<TAB>ok" or "path<TAB>reason". Absent: all movable.
                Its reasons become the requirement conflicts.
  references    list of reference patterns with {path}/{stem}/{title}
                placeholders; on apply, a line matching a pattern that
                names a moved file is repointed to the new path.
  validate      command run in the tree after staging (exit 0 = valid);
                absent: no validation.
  root          the tree root the adapter governs; defaults to the
                config file's directory.
"""
import glob
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

import organize_core
from organize_core import (Vote, Placement, Verdict, Step, Plan,
                          ApplyResult, Desire)


def _load_config(path):
    """Read the JSON config and resolve its relative paths against the
    config file's directory. root defaults to that directory."""
    with open(path, encoding="utf-8") as fh:
        cfg = dict(json.load(fh))
    base = os.path.dirname(os.path.abspath(path))
    cfg["_base"] = base
    root = cfg.get("root")
    cfg["root"] = os.path.abspath(os.path.join(base, root)) if root else base
    # The map is passed to the core, which reads it verbatim, so resolve
    # it to an absolute path against the config's directory here.
    if cfg.get("map"):
        cfg["map"] = os.path.abspath(os.path.join(base, cfg["map"]))
    return cfg


def _resolve(cfg, value):
    """A config path or command, resolved against the config directory.

    A bare word that names a file next to the config becomes its
    absolute path; anything else (an absolute path, a multi-word command,
    a PATH executable) passes through untouched, so `validate: linkcheck`
    and `tagCmd: ./tag.sh` both work."""
    if value is None:
        return None
    parts = shlex.split(value)
    if len(parts) == 1:
        cand = os.path.join(cfg["_base"], parts[0])
        if os.path.exists(cand):
            return [os.path.abspath(cand)]
    if len(parts) == 1 and (os.sep in parts[0] or parts[0].startswith(".")):
        return [os.path.join(cfg["_base"], parts[0])]
    return parts


def _run_batch(argv, paths, root):
    """The batched command protocol: write NUL-separated paths to the
    command's stdin once, read "path<TAB>result" lines from its stdout.

    Returns {path: result}. A path the command does not name is absent
    from the map, which the caller reads as its default (movable, no
    tag). Exit nonzero is loud, so a broken call-out fails the run rather
    than silently dropping every file."""
    if not argv:
        return {}
    r = subprocess.run(argv, cwd=root, input="\0".join(sorted(paths)) + "\0",
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"organize: command {argv[0]} failed: {r.stderr.strip()}")
    out = {}
    for line in r.stdout.splitlines():
        if "\t" not in line:
            continue
        path, result = line.split("\t", 1)
        out[path] = result
    return out


class ConfigInterpreter:
    """State each file's Desire from the tag command (or a map-token
    fallback) plus the declared layout map and command-supplied
    overrides, folded into one placement.

    The tag command speaks the tag protocol: for each file it emits zero
    or more "path<TAB>key=value" lines with keys area, role, kind. The
    area key is the placement label; role and kind ride in the vote's
    distribution. With no tag command, a file's area is the first map
    token whose name appears as a path component, so a plain map is
    enough for a flat tree. The map is "directory: token token" per
    line, the same format the git adapter's layout map uses; a token
    routes to its directory. An override, from the overrides command, is
    a declared area at highest precedence."""

    def __init__(self, cfg):
        self._cfg = cfg
        self._owner = {}
        self._name = cfg.get("name", "config layout")
        self._tag_cmd = _resolve(cfg, cfg.get("tagCmd"))
        self._over_cmd = _resolve(cfg, cfg.get("overridesCmd"))
        self._root = cfg["root"]

    # the Interpreter contract

    def desires(self, scope):
        """{FileId: Desire} for the files the label or an override names.

        A file with a label or a declared override is present; one with
        neither is absent. A named file whose label resolves to no target
        is present with Desire(place=None). hold stays None."""
        votes = self._label(scope)
        over = self._overrides(scope)
        out = {}
        for f in scope:
            vote = votes.get(f)
            ov = over.get(f)
            if (vote is None or vote.primary is None) and ov is None:
                continue
            p = self._place(f, vote, ov)
            out[f] = Desire(place=p.target, hold=None)
        return out

    def load(self, mapref):
        self._name = os.path.basename(mapref)
        owner = {}
        try:
            fh = open(mapref, encoding="utf-8")
        except FileNotFoundError:
            sys.exit(f"organize: no such map file: {mapref}")
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line or ":" not in line:
                continue
            d, tokens = line.split(":", 1)
            for tok in tokens.split():
                owner[tok] = d.strip()
        self._owner = owner

    def name(self):
        return self._name

    def target_of(self, label):
        """A label to its target directory. Accepts a map token, a
        multi-part label, or the destination directory name itself, so
        an override may name the directory. Unresolved returns None."""
        if label in self._owner.values():
            return label
        return self._owner.get(self._token_of(label))

    def ordered_targets(self):
        return list(dict.fromkeys(self._owner.values()))

    def token_for(self, target):
        """A label the map routes to target, for suggesting an override.
        Prefers the target's own name when the map owns it, else the
        first token that maps there."""
        if self._owner.get(target) == target:
            return target
        for tok, d in self._owner.items():
            if d == target:
                return tok
        return target

    # the label, override, and place internals

    def _tag_lines(self, scope):
        """{path: {key: value}} from the tag command, keys area/role/kind."""
        raw = _run_batch(self._tag_cmd, scope, self._root)
        tags = {}
        for path, kv in raw.items():
            if "=" in kv:
                key, value = kv.split("=", 1)
                tags.setdefault(path, {})[key.strip()] = value.strip()
        return tags

    def _label(self, scope):
        votes = {}
        if self._tag_cmd:
            for f, kv in self._tag_lines(scope).items():
                if f not in scope:
                    continue
                area = kv.get("area")
                if not area:
                    continue
                dist = {area: 1.0}
                for k in ("role", "kind"):
                    if kv.get(k):
                        dist[f"{k}:{kv[k]}"] = 1.0
                votes[f] = Vote(dist=dist, primary=area,
                                confidence=1.0, status="labelled")
            return votes
        # Fallback: the map token that appears in the file's path.
        for f in scope:
            token = self._token_in_path(f)
            if token:
                votes[f] = Vote(dist={token: 1.0}, primary=token,
                                confidence=1.0, status="labelled")
        return votes

    def _token_of(self, label):
        """The map token for a label: the first hyphen-free segment of
        the first path component, so guide/api and guide-v2 both reduce
        to guide."""
        return label.split("/")[0].split("-")[0]

    def _token_in_path(self, path):
        """The first map token that appears as a component of path, for
        the no-tag-command fallback. None when no token matches."""
        parts = re.split(r"[\\/._-]", path.lower())
        for tok in self._owner:
            if tok.lower() in parts:
                return tok
        return None

    def _overrides(self, scope):
        """{path: area} declared by the overrides command, for scope
        paths that set an area= value. Absent command: no overrides."""
        if not self._over_cmd:
            return {}
        over = {}
        for path, result in _run_batch(self._over_cmd, scope,
                                        self._root).items():
            if path not in scope or "=" not in result:
                continue
            key, value = result.split("=", 1)
            if key.strip() == "area" and value.strip():
                over[path] = value.strip()
        return over

    def _place(self, f, vote, override):
        if override is not None:
            label, reason = override, "override"
        elif vote is not None and vote.primary is not None:
            label, reason = vote.primary, "label"
        else:
            return Placement(target=None, label=None, reason="none")
        return Placement(target=self.target_of(label),
                         label=label, reason=reason)


class ConfigTransformer:
    """The generic move cascade: scope from member globs, relocatability
    from a command, and plan/apply built from the primitives.

    The primitives are mkdir the target, mv the file, repoint each
    reference the config's patterns match, run the validate command, and
    report. Only the reference repoint reads a domain specific, and that
    specific is a pattern string in the config, not a literal here."""

    def __init__(self, cfg):
        self._cfg = cfg
        self._root = cfg["root"]
        self._members = cfg.get("members", [])
        self._reloc_cmd = _resolve(cfg, cfg.get("relocateCmd"))
        self._validate_cmd = _resolve(cfg, cfg.get("validate"))
        self._patterns = cfg.get("references", [])
        self._reloc = None         # cached {path: reason} from relocateCmd

    # scope and readiness

    def scope(self):
        """The files any member glob matches, as paths relative to the
        tree root. A recursive glob (one with a double-star segment)
        descends; a target directory a prior apply created is still in
        scope, so a second run sees a clean tree."""
        found = set()
        for pat in self._members:
            for p in glob.glob(os.path.join(self._root, pat),
                               recursive=True):
                if os.path.isfile(p):
                    found.add(os.path.relpath(p, self._root))
        return found

    def target_ready(self, target):
        return os.path.isdir(os.path.join(self._root, target))

    def already_at(self, f, target):
        return os.path.dirname(f) == target

    def is_source(self, f):
        """Every member is a source; the generic adapter has no riders.
        A domain that pairs files supplies a plugin."""
        return True

    def paired_internal_header(self, f):
        """No paired-file convention in the generic adapter; a domain
        that pairs files (a source and its header) supplies a plugin."""
        return None

    def preflight(self):
        return []

    def recover_hint(self):
        return "recover the pre-apply tree by moving the files back"

    # relocatability

    def _relocatability(self, scope):
        """{path: reason} for the files the relocate command blocks.

        The command speaks the predictive-check protocol: a path it names
        with a reason cannot move (a requirement conflict); a path it
        names "ok", or does not name, is movable. Absent command: every
        file moves. Cached so plan runs it once."""
        if self._reloc is None:
            if not self._reloc_cmd:
                self._reloc = {}
            else:
                self._reloc = {
                    p: r for p, r in
                    _run_batch(self._reloc_cmd, scope, self._root).items()
                    if r.strip() and r.strip() != "ok"}
        return self._reloc

    # plan

    def plan(self, target, files):
        """One target's Plan: the movable files become moves, the blocked
        files become skipped requirement conflicts. Drops files already
        in target. No pairing and no build-list edit; the reference
        repoint runs in apply from the config patterns."""
        flagged = {f for f in files if not self.already_at(f, target)}
        blocked = self._relocatability(self.scope())
        move_set, skipped = set(), []
        for f in sorted(flagged):
            reason = blocked.get(f)
            if reason:
                skipped.append((f, reason))
            else:
                move_set.add(f)
        moves = tuple(sorted((f, f"{target}/{os.path.basename(f)}")
                             for f in move_set))
        steps = tuple(
            Step(kind="move", summary=f"  {src}  ->  {dst}",
                 reads=(src,), writes=(dst,), preview=None,
                 payload=(src, dst))
            for src, dst in moves)
        notes = self._notes(moves)
        return Plan(target=target, moves=moves,
                    skipped=tuple(sorted(skipped)), steps=steps,
                    notes=notes, kept_public=(), paired_headers=0)

    def _notes(self, moves):
        if not moves:
            return ()
        lines = ["", f"{len(moves)} files move by rename"]
        if self._patterns:
            lines.append(f"references repointed by "
                         f"{len(self._patterns)} pattern(s)")
        if self._validate_cmd:
            lines.append("validated after staging")
        return tuple(lines)

    # apply

    def apply(self, plan, commit):
        """Perform one target's cascade: mkdir the target, mv each file,
        repoint the references the config patterns match, run validate,
        and report. commit is accepted for the protocol; this adapter
        leaves the tree in place for review rather than committing."""
        return self._run([plan])

    def apply_auto(self, plans, commit):
        """Perform several targets as one operation, the same cascade."""
        return self._run(plans)

    def _run(self, plans):
        lines = []
        all_moves = [m for p in plans for m in p.moves]
        if not all_moves:
            return ApplyResult(True, ["organize apply: nothing to do"])
        # Create every target first, so a mv never races an absent dir.
        for p in plans:
            Path(self._root, p.target).mkdir(parents=True, exist_ok=True)
        # mv each file.
        for src, dst in all_moves:
            src_abs = os.path.join(self._root, src)
            dst_abs = os.path.join(self._root, dst)
            if os.path.exists(dst_abs):
                lines.append(f"organize apply: {dst} already exists; "
                             "refusing to overwrite")
                lines.append(self.recover_hint())
                return ApplyResult(False, lines)
            os.rename(src_abs, dst_abs)
        # repoint every reference that names a moved file.
        n_ref = self._repoint(all_moves)
        # validate the staged tree.
        v = self.validate()
        if not v.ok:
            lines.append(f"organize apply: {v.reason}; nothing committed.")
            lines.append(self.recover_hint())
            return ApplyResult(False, lines)
        lines.append(f"organize apply: moved {len(all_moves)} files, "
                     f"repointed {n_ref} reference line(s).")
        if v.reason:
            lines.append(v.reason)
        lines.append("review the tree; the moves are on disk.")
        return ApplyResult(True, lines)

    def _repoint(self, moves):
        """Repoint every reference line that a config pattern matches and
        that names a moved file, to the moved file's new path.

        A pattern is a template with a {path}, {stem}, or {title}
        placeholder; the config supplies the exact literal syntax. For
        each moved file this builds the pattern's matcher, scans every
        scope file plus the moved files, and, on a line that names the
        old path (or its basename), rewrites the path to a route from the
        referring file to the new location. Returns the count of
        rewritten lines."""
        if not self._patterns:
            return 0
        # Map each moved file's possible reference spellings to its new
        # path, so a link by basename or by relative path both repoint.
        old_to_new = {}
        for src, dst in moves:
            for spelling in self._reference_spellings(src):
                old_to_new[spelling] = dst
        files = self.scope() | {dst for _s, dst in moves}
        rewritten = 0
        for f in sorted(files):
            fp = os.path.join(self._root, f)
            if not os.path.isfile(fp):
                continue
            text = open(fp, encoding="utf-8").read()
            new_text, hits = self._repoint_text(f, text, moves)
            if hits:
                open(fp, "w", encoding="utf-8").write(new_text)
                rewritten += hits
        return rewritten

    def _reference_spellings(self, path):
        """The strings a reference might use to name path: its full
        tree-relative path and its basename."""
        return {path, os.path.basename(path)}

    def _pattern_regex(self, template):
        """Compile a reference template into a regex with a named 'path'
        group, escaping the literal text and turning the placeholders
        into captures. {path} and {stem} capture the referenced target;
        {title} captures free text (a link title). Returns the compiled
        regex, or None when the template names no path."""
        # Split on the placeholders, keeping them, so literal runs are
        # escaped and placeholders become groups.
        parts = re.split(r"(\{path\}|\{stem\}|\{title\})", template)
        out = []
        for part in parts:
            if part == "{path}":
                out.append(r"(?P<path>[^)\"'\s]+)")
            elif part == "{stem}":
                out.append(r"(?P<path>[^)\"'\s.]+)")
            elif part == "{title}":
                out.append(r"[^\]]*")
            elif part:
                out.append(re.escape(part))
        if "path" not in "".join(p for p in parts if p in
                                 ("{path}", "{stem}")):
            return None
        return re.compile("".join(out))

    def _repoint_text(self, referring_file, text, moves):
        """Rewrite, in one file's text, every reference whose captured
        path names a moved file. The replacement path is a route from the
        referring file's directory to the moved file's new location, so a
        relative link stays relative. Returns (new_text, hit_count)."""
        move_by_old = {}
        for src, dst in moves:
            for spelling in self._reference_spellings(src):
                move_by_old[spelling] = (src, dst)
        hits = [0]
        ref_dir = os.path.dirname(referring_file)

        def repoint_one(regex):
            def sub(m):
                captured = m.group("path")
                key = self._match_moved(captured, move_by_old)
                if key is None:
                    return m.group(0)
                src, dst = move_by_old[key]
                new_rel = os.path.relpath(dst, ref_dir) if ref_dir else dst
                hits[0] += 1
                return m.group(0).replace(captured, new_rel)
            return sub

        new_text = text
        for template in self._patterns:
            regex = self._pattern_regex(template)
            if regex is None:
                continue
            new_text = regex.sub(repoint_one(regex), new_text)
        return new_text, hits[0]

    def _match_moved(self, captured, move_by_old):
        """The move key a captured reference names, resolving a relative
        or bare path against the moved files. None when it names none."""
        if captured in move_by_old:
            return captured
        base = os.path.basename(captured)
        if base in move_by_old:
            return base
        norm = os.path.normpath(captured)
        if norm in move_by_old:
            return norm
        return None

    def validate(self):
        """Run the validate command in the tree; exit 0 means the
        artifact still holds. Absent command: valid with no check. A
        domain plugs its checker here (a link checker, a build)."""
        if not self._validate_cmd:
            return Verdict(True, "no validate command; skipped")
        r = subprocess.run(self._validate_cmd, cwd=self._root,
                           capture_output=True, text=True)
        if r.returncode:
            detail = (r.stderr or r.stdout).strip()
            return Verdict(False, f"validate failed: {detail}")
        return Verdict(True, "validate passed")


def register_config(path):
    """Load a config file and register its triple under the config's
    name. The launcher calls this for --config FILE; the core then
    dispatches to it like any other triple."""
    cfg = _load_config(path)
    name = cfg.get("name", "config")

    def build():
        interp = ConfigInterpreter(cfg)
        transformer = ConfigTransformer(cfg)
        return (interp, transformer)

    organize_core.register(name, build)
    return name, cfg


# The generic dispatch. The config adapter keeps its own status summary
# and all-areas apply so the "config layout clean" phrasing and the
# already/untracked counts stay stable; status <area> and apply --area X
# read from the plan, so they defer to the core unchanged.


def _config_status(interp, transformer, mapref):
    """The state summary for a config pair, built from the generic
    seams: renamed is the files a plan would move, conflict is the
    requirement conflicts the relocate command flags, clean is the
    files already in place."""
    interp.load(mapref)
    renamed, conflict, rows = 0, 0, []
    scope = transformer.scope()
    placed = organize_core._desires(interp, transformer)
    placed_files = set(placed)
    for t in interp.ordered_targets():
        files = organize_core._files_for(interp, transformer, t)
        if not files:
            continue
        plan = transformer.plan(t, files)
        state = "exists" if transformer.target_ready(t) else "new"
        rows.append((t, state, len(plan.moves), len(plan.skipped)))
        renamed += len(plan.moves)
        conflict += len(plan.skipped)
    already = len(placed_files) - renamed - conflict
    untracked = len(scope - placed_files)
    clean_layout = renamed == 0 and conflict == 0
    banner = "(layout clean)" if clean_layout else "(layout not clean)"
    print(f"organize status against {interp.name()}   {banner}\n")
    print(f"renamed    {renamed:<5} will move on apply")
    print(f"conflict   {conflict:<5} cannot auto-apply "
          "(requirement: a check blocks the move)")
    print(f"clean      {already:<5} already in their area")
    print(f"untracked  {untracked:<5} no rule places these\n")
    for t, state, n_move, n_skip in rows:
        print(f"  {t + '/':<14}{state:<7}{n_move:>3} move"
              f"{'':>4}{n_skip:>3} conflict")
    print("\napply --unconflicted moves every unconflicted file in one\n"
          "validated pass; status <area> shows one area's move set.")


def _config_apply_auto(interp, transformer, mapref):
    """All-areas apply for a config pair. Collect every area's plan,
    then run them as one operation through the generic apply_auto, so a
    cross-area reference repoints against the whole batch and validate
    sees the fully moved tree. Routes through the transformer directly."""
    interp.load(mapref)
    blockers = transformer.preflight()
    if blockers:
        sys.exit("organize apply --unconflicted: " + "; ".join(blockers))
    plans = []
    for t in interp.ordered_targets():
        files = organize_core._files_for(interp, transformer, t)
        if not files:
            continue
        plan = transformer.plan(t, files)
        if plan.moves:
            plans.append(plan)
    if not plans:
        print("organize apply --unconflicted: nothing to carve")
        return
    n = sum(len(p.moves) for p in plans)
    held = sum(len(p.skipped) for p in plans)
    print(f"converging {len(plans)} areas, {n} files; holding {held} "
          f"conflict sources at root\n")
    result = transformer.apply_auto(plans, False)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def dispatch(name, mapref, argv):
    """The launcher entry point for a config pair. Route the two
    git-coupled commands (the default status summary and
    apply --unconflicted) through the generic seams; defer every other
    command to the core, which handles a config pair unchanged.

    argv is the argument list with --config already removed. A --map
    override, if present, replaces the config's map."""
    probe, argv = list(argv), list(argv)
    map_override, probe = organize_core.take_opt(probe, "--map")
    if map_override is not None:
        mapref = map_override
    area, probe = organize_core.take_opt(probe, "--area")
    auto = "--unconflicted" in probe
    words = [a for a in probe if not a.startswith("-")]
    cmd = words[0] if words else "status"
    interp, transformer = organize_core.get_triple(name)
    flags = ("--by-area", "--conflicts", "--exit-code")
    named_area = area or (words[1] if cmd == "status" and len(words) > 1
                          else None)
    if cmd == "status" and named_area is None \
            and not any(f in probe for f in flags):
        _config_status(interp, transformer, mapref)
        return
    if cmd == "apply" and auto:
        _config_apply_auto(interp, transformer, mapref)
        return
    # Every other command (status <area>, apply --area X, status
    # --by-area) reads from the plan, so the core handles it with no
    # second signal. The core re-reads sys.argv, so leave it untouched
    # and pass the defaults.
    organize_core.main(default_triple=name, default_mapref=mapref)
