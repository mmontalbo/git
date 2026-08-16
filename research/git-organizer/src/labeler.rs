//! The labeler: mine each file's subsystem from its history. Fold in the
//! declared overrides and the C role/pairing, then name a target dir.

use crate::git;
use crate::layout::AreaMap;
use crate::model::Model;
use crate::types::ROOT;
use crate::util::{dirname, norm, stem};
use anyhow::Result;
use regex::Regex;
use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::sync::LazyLock;

static PRE_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^([A-Za-z0-9][\w./-]*):").unwrap());

/// Flat root `*.c` and `*.h` tracked files.
pub fn scope(top: &Path) -> Result<HashSet<String>> {
    Ok(git::run(top, &["ls-files", "*.c", "*.h"], &[0])?
        .split_whitespace()
        .filter(|f| !f.contains('/'))
        .map(str::to_string)
        .collect())
}

/// Fold one commit's contribution into the per-file, per-label weights.
fn accumulate(
    wt: &mut HashMap<String, HashMap<String, f64>>,
    label: &Option<String>,
    touched: &HashSet<String>,
    total: u64,
) {
    let Some(label) = label else { return };
    if touched.is_empty() || total == 0 {
        return;
    }
    let w = 1.0 / total as f64;
    for f in touched {
        *wt.entry(f.clone()).or_default().entry(label.clone()).or_insert(0.0) += w;
    }
}

/// Label each file by the modal commit-subject prefix over its history.
/// A file below either evidence threshold (2.0 support, 0.34 modal share)
/// is absent.
fn commit_prefix(top: &Path, scope: &HashSet<String>) -> Result<HashMap<String, String>> {
    let log = git::run(
        top,
        &["log", "--no-merges", "--name-only", "--format=%x00%s"],
        &[0],
    )?;

    let mut wt: HashMap<String, HashMap<String, f64>> = HashMap::new();
    let mut label: Option<String> = None;
    let mut touched: HashSet<String> = HashSet::new();
    let mut total: u64 = 0;
    for line in log.split('\n') {
        if let Some(subject) = line.strip_prefix('\0') {
            accumulate(&mut wt, &label, &touched, total);
            label = PRE_RE.captures(subject).map(|c| norm(&c[1]));
            touched.clear();
            total = 0;
        } else if !line.is_empty() {
            total += 1;
            if scope.contains(line) {
                touched.insert(line.to_string());
            }
        }
    }
    accumulate(&mut wt, &label, &touched, total);

    let mut labels = HashMap::new();
    for (f, counts) in &wt {
        let tot: f64 = counts.values().sum();
        let best = counts
            .iter()
            .max_by(|a, b| a.1.partial_cmp(b.1).unwrap().then_with(|| a.0.cmp(b.0)));
        if let Some((label, &w)) = best {
            if tot >= 2.0 && w / tot >= 0.34 {
                labels.insert(f.clone(), label.clone());
            }
        }
    }
    Ok(labels)
}

/// `path -> declared organize.subsystem value` for scope paths that set
/// it, from one batched `git check-attr`.
fn overrides(top: &Path, scope: &HashSet<String>) -> Result<HashMap<String, String>> {
    if scope.is_empty() {
        return Ok(HashMap::new());
    }
    let mut files: Vec<&String> = scope.iter().collect();
    files.sort();
    let mut input = Vec::new();
    for f in &files {
        input.extend_from_slice(f.as_bytes());
        input.push(0);
    }
    let out = git::run_with_stdin(
        top,
        &["check-attr", "organize.subsystem", "--stdin", "-z"],
        &input,
    )?;
    let fields: Vec<&str> = out.split('\0').collect();
    let mut over = HashMap::new();
    let mut i = 0;
    while i + 2 < fields.len() {
        let (path, value) = (fields[i], fields[i + 2]);
        if !value.is_empty() && value != "unspecified" {
            over.insert(path.to_string(), value.to_string());
        }
        i += 3;
    }
    Ok(over)
}

/// The final `{file: target dir}` labels: mine the root sources, then
/// overlay the declared attributes (the read-back). A declared source,
/// and any file the miner did not place, take the declaration. A header
/// the miner already role-folded keeps that placement, so a public header
/// stays at the root and its includes do not break. This is the whole
/// label decision; the labeler binary serializes the result.
pub fn all_labels(model: &Model, map: &AreaMap) -> Result<HashMap<String, String>> {
    let files = scope(&model.top)?;
    let mut labels = compute_labels(model, map, &files)?;
    for (f, t) in read_declared(model, map)? {
        if f.ends_with(".c") || !labels.contains_key(&f) {
            labels.insert(f, t);
        }
    }
    Ok(labels)
}

/// `{file: target dir}` for every tracked `.c`/`.h` carrying an organize
/// declaration: `organize.subsystem` names its dir, `organize.role=public`
/// keeps it at the root. The declaration is authoritative, so a carved or
/// misplaced file reconciles against it instead of the miner.
pub fn read_declared(model: &Model, map: &AreaMap) -> Result<HashMap<String, String>> {
    let listing = git::run(&model.top, &["ls-files", "*.c", "*.h"], &[0])?;
    let files: Vec<&str> = listing.split_whitespace().collect();
    if files.is_empty() {
        return Ok(HashMap::new());
    }
    let mut input = Vec::new();
    for f in &files {
        input.extend_from_slice(f.as_bytes());
        input.push(0);
    }
    let out = git::run_with_stdin(
        &model.top,
        &["check-attr", "organize.subsystem", "organize.role", "--stdin", "-z"],
        &input,
    )?;
    // The -z stream is (path, attr, value) triples, two attrs per file.
    let fields: Vec<&str> = out.split('\0').collect();
    let mut subsystem: HashMap<&str, &str> = HashMap::new();
    let mut role: HashMap<&str, &str> = HashMap::new();
    let mut i = 0;
    while i + 2 < fields.len() {
        let (path, attr, value) = (fields[i], fields[i + 1], fields[i + 2]);
        if !value.is_empty() && value != "unspecified" {
            match attr {
                "organize.subsystem" => {
                    subsystem.insert(path, value);
                }
                "organize.role" => {
                    role.insert(path, value);
                }
                _ => {}
            }
        }
        i += 3;
    }
    let mut declared = HashMap::new();
    for f in &files {
        if let Some(v) = subsystem.get(f) {
            if let Some(t) = map.target_of(v) {
                declared.insert((*f).to_string(), t);
            }
        } else if role.get(f) == Some(&"public") {
            declared.insert((*f).to_string(), ROOT.to_string());
        }
    }
    Ok(declared)
}

/// `{file: target dir}` labeling each root `.c`/`.h` with its subsystem,
/// folding role and pairing on top of the source placements.
pub fn compute_labels(
    model: &Model,
    map: &AreaMap,
    scope: &HashSet<String>,
) -> Result<HashMap<String, String>> {
    let prefixes = commit_prefix(&model.top, scope)?;
    let over = overrides(&model.top, scope)?;

    // The prefix-or-override target for a file (override wins), or None.
    let base_place = |f: &str| -> Option<String> {
        let chosen = over.get(f).or_else(|| prefixes.get(f))?;
        map.target_of(chosen)
    };

    // Source placements: a .c that resolves to a dir targets it.
    let mut c_target: HashMap<String, String> = HashMap::new();
    for c in scope {
        if c.ends_with(".c") {
            if let Some(t) = base_place(c) {
                c_target.insert(c.clone(), t);
            }
        }
    }

    // The .c files that actually move (library objects not already home).
    let mut moving_c: HashSet<String> = HashSet::new();
    for (c, t) in &c_target {
        if model.is_lib_object(c) && dirname(c) != t.as_str() {
            moving_c.insert(c.clone());
        }
    }

    // Candidate headers: a same-stem .h of a moving .c, or a flagged
    // header-only lib (no same-stem .c).
    let mut cands: HashSet<String> = HashSet::new();
    for c in &moving_c {
        let h = format!("{}.h", stem(c));
        if model.exists(&h) {
            cands.insert(h);
        }
    }
    for h in scope {
        if h.ends_with(".h")
            && base_place(h).is_some()
            && !model.exists(&format!("{}.c", stem(h)))
        {
            cands.insert(h.clone());
        }
    }

    // Each candidate's home dir, and dir_of for the moving sources.
    let mut home: HashMap<String, String> = HashMap::new();
    for h in &cands {
        let c = format!("{}.c", stem(h));
        let target = match c_target.get(&c) {
            Some(t) => Some(t.clone()),
            None => base_place(h),
        };
        if let Some(t) = target {
            home.insert(h.clone(), t);
        }
    }
    let dir_of: HashMap<String, String> = moving_c
        .iter()
        .map(|c| (c.clone(), c_target[c].clone()))
        .collect();
    let internal = model.internal_headers(&cands, &home, &dir_of)?;

    // Fold the header roles onto the source placements. Reuse c_target as
    // the base (no clone); a header rides its source (internal) or is
    // kept at the root (public).
    let mut out = c_target;
    for h in scope {
        if !h.ends_with(".h") {
            continue;
        }
        let c = format!("{}.c", stem(h));
        if model.exists(&c) {
            // A same-stem header: absent if its source is unmanaged,
            // else internal -> the source's dir, public -> ROOT.
            if let Some(t) = out.get(&c).cloned() {
                let target = if internal.contains(h) { t } else { ROOT.to_string() };
                out.insert(h.clone(), target);
            }
        } else if internal.contains(h) {
            // A header-only lib that all includers co-move with.
            if let Some(t) = base_place(h) {
                out.insert(h.clone(), t);
            }
        }
    }
    Ok(out)
}
