//! The transformer: turn the labels into renames plus the Makefile and
//! meson build-list patches. Compute each patch's new bytes.

use crate::model::Model;
use crate::types::{Diff, Rename, HOLD_PROGRAM, ROOT};
use crate::util::{basename, dirname};
use anyhow::{ensure, Context, Result};
use regex::{Captures, Regex};
use std::collections::{BTreeMap, HashMap};
use std::fs;

/// The tree diff: a rename per file whose target path differs (a program
/// is held as a conflict), plus the moved `.c` basenames grouped by
/// target dir for the build-list patches.
pub fn compute_diff(model: &Model, labels: &HashMap<String, String>) -> Diff {
    let mut renames = Vec::new();
    for (f, t) in labels {
        // The target path keys on the basename, so a file already in a
        // subsystem dir (declared, carved) reconciles against its target:
        // it is in place when the dir matches, and a move otherwise.
        let dst = if t == ROOT {
            basename(f).to_string()
        } else {
            format!("{t}/{}", basename(f))
        };
        if dst == *f {
            continue;
        }
        let (ok, reason) = if f.ends_with(".c") && !model.is_lib_object(f) {
            (false, HOLD_PROGRAM.to_string())
        } else {
            (true, String::new())
        };
        renames.push(Rename { src: f.clone(), dst, ok, reason });
    }
    renames.sort_by(|a, b| a.src.cmp(&b.src));

    // The build-list patches reparent a root source's LIB_OBJS line, which
    // only exists for a source at the root, so group only the root moves.
    let mut moved: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for r in &renames {
        if r.ok && r.src.ends_with(".c") && !r.src.contains('/') {
            moved
                .entry(dirname(&r.dst).to_string())
                .or_default()
                .push(basename(&r.src).to_string());
        }
    }
    Diff { renames, moved }
}

/// The referrer's new text after the build patch, computed without
/// touching the tree.
pub fn patch_content(
    model: &Model,
    referrer: &str,
    moved: &BTreeMap<String, Vec<String>>,
) -> Result<String> {
    let mut text = fs::read_to_string(model.top.join(referrer))
        .with_context(|| format!("reading {referrer}"))?;
    for (target, names) in moved {
        match referrer {
            "Makefile" => {
                text = make_libobjs(text, names, target)?;
                text = make_object_rules(text, names, target)?;
            }
            "meson.build" => text = meson(text, names, target)?,
            _ => {}
        }
    }
    Ok(text)
}

/// The object stems (`<name>.c` -> `<name>`) of a moved group.
fn stems(moved_c: &[String]) -> Vec<&str> {
    moved_c.iter().map(|c| &c[..c.len() - 2]).collect()
}

/// One alternation over the escaped stems, e.g. `alias|add-patch`.
fn alternation(names: &[&str]) -> String {
    names.iter().map(|n| regex::escape(n)).collect::<Vec<_>>().join("|")
}

/// Count matches of a group's second capture (the stem) per name.
fn counts_by_name(re: &Regex, text: &str) -> HashMap<String, usize> {
    let mut counts = HashMap::new();
    for cap in re.captures_iter(text) {
        *counts.entry(cap[2].to_string()).or_insert(0) += 1;
    }
    counts
}

/// Reparent the `LIB_OBJS` line and, when present, the `LOCALIZED_C_CORE`
/// line for each moved `.c` (one line per name, 0 or 1 localized line).
fn make_libobjs(text: String, moved_c: &[String], target: &str) -> Result<String> {
    let names = stems(moved_c);
    let alt = alternation(&names);

    let lib = Regex::new(&format!(r"(?m)^([ \t]*)LIB_OBJS \+= ({alt})\.o$")).unwrap();
    let counts = counts_by_name(&lib, &text);
    for n in &names {
        let got = counts.get(*n).copied().unwrap_or(0);
        ensure!(
            got == 1,
            "organize apply: Makefile: expected 1 line for {n}, found {got}"
        );
    }
    let text = lib
        .replace_all(&text, |c: &Captures| {
            format!("{}LIB_OBJS += {}/{}.o", &c[1], target, &c[2])
        })
        .into_owned();

    let localized = Regex::new(&format!(r"(?m)^([ \t]*)LOCALIZED_C_CORE \+= ({alt})\.c$")).unwrap();
    let counts = counts_by_name(&localized, &text);
    for n in &names {
        let got = counts.get(*n).copied().unwrap_or(0);
        ensure!(
            got <= 1,
            "organize apply: Makefile: expected 0 or 1 LOCALIZED_C_CORE line for {n}, found {got}"
        );
    }
    Ok(localized
        .replace_all(&text, |c: &Captures| {
            format!("{}LOCALIZED_C_CORE += {}/{}.c", &c[1], target, &c[2])
        })
        .into_owned())
}

/// Reparent the per-object `NAME.sp NAME.s NAME.o:` triplet rules for the
/// moved sources, then confirm no root object rule survives.
fn make_object_rules(text: String, moved_c: &[String], target: &str) -> Result<String> {
    let names = stems(moved_c);
    let alt = alternation(&names);
    let pfx = format!("{target}/");

    let re = Regex::new(&format!(
        r"(?m)^(?P<lead>[ \t]*)(?P<n1>{alt})\.sp[ \t]+(?P<n2>{alt})\.s[ \t]+(?P<n3>{alt})\.o(?P<rest>[ \t]*:.*)$"
    ))
    .unwrap();
    let text = re
        .replace_all(&text, |c: &Captures| {
            let (n1, n2, n3) = (&c["n1"], &c["n2"], &c["n3"]);
            if n1 == n2 && n2 == n3 {
                format!("{}{pfx}{n1}.sp {pfx}{n1}.s {pfx}{n1}.o{}", &c["lead"], &c["rest"])
            } else {
                // A cross-name line no single-name rule would touch.
                c[0].to_string()
            }
        })
        .into_owned();

    for n in &names {
        ensure!(
            !root_object_rule(&text, n),
            "organize apply: Makefile: a per-object rule for {n}.o survives in a form this \
             adapter does not reparent; refusing to move {n}.c"
        );
    }
    Ok(text)
}

/// Whether `<name>.o` appears as a rule target (before the first colon of
/// some line) as a token not bounded by `[\w./-]`. Hand-rolled because the
/// `regex` crate has no look-around.
fn root_object_rule(text: &str, name: &str) -> bool {
    let needle = format!("{name}.o");
    let is_boundary = |b: u8| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'.' | b'/' | b'-');
    for line in text.lines() {
        let Some(colon) = line.find(':') else {
            continue;
        };
        let head = &line[..colon];
        let bytes = head.as_bytes();
        let mut start = 0;
        while let Some(pos) = head[start..].find(needle.as_str()) {
            let idx = start + pos;
            let before_ok = idx == 0 || !is_boundary(bytes[idx - 1]);
            let after = idx + needle.len();
            let after_ok = after >= bytes.len() || !is_boundary(bytes[after]);
            if before_ok && after_ok {
                return true;
            }
            start = idx + 1;
        }
    }
    false
}

/// Reparent each moved `.c` source line in `meson.build` (one per name).
fn meson(text: String, moved_c: &[String], target: &str) -> Result<String> {
    let names = stems(moved_c);
    let alt = alternation(&names);
    let re = Regex::new(&format!(r"(?m)^([ \t]*)'({alt})\.c',$")).unwrap();
    let counts = counts_by_name(&re, &text);
    for n in &names {
        let got = counts.get(*n).copied().unwrap_or(0);
        ensure!(
            got == 1,
            "organize apply: meson.build: expected 1 line for {n}, found {got}"
        );
    }
    Ok(re
        .replace_all(&text, |c: &Captures| format!("{}'{}/{}.c',", &c[1], target, &c[2]))
        .into_owned())
}
