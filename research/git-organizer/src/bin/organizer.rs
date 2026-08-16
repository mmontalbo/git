//! git's organizer: read label records on stdin, emit mv and patch ops.
//!
//! Input is one `path \0 key \0 value` record per line, the label records
//! the git organize builtin reconciles (`subsystem <dir>`, or `role
//! public`). Output is the op stream the builtin performs: a rename per
//! file whose location differs from its label, plus the Makefile and meson
//! build-list patches those moves entail. It reads the worktree but never
//! touches .gitattributes; the builtin owns the declaration.
//!
//!   rename:  R \0 <ok> \0 <label> \0 <src> \0 <dst> \0 <reason> \n
//!   patch:   P \0 <path> \0 <nbytes> \n <nbytes bytes of content>

use anyhow::{bail, Result};
use organize::model::Model;
use organize::transform::{compute_diff, patch_content};
use organize::types::ROOT;
use organize::git;
use std::collections::HashMap;
use std::io::{BufWriter, Read, Write};
use std::process::ExitCode;

/// Parse the `path \0 key \0 value` label stream into `{path: target dir}`.
fn read_labels(input: &str) -> Result<HashMap<String, String>> {
    let mut labels = HashMap::new();
    for line in input.lines() {
        if line.is_empty() {
            continue;
        }
        let fields: Vec<&str> = line.split('\0').collect();
        let [path, key, value] = fields[..] else {
            bail!("organizer: malformed label record: {line:?}");
        };
        let target = match (key, value) {
            ("subsystem", dir) => dir.to_string(),
            ("role", "public") => ROOT.to_string(),
            _ => bail!("organizer: unknown label {key}={value}"),
        };
        labels.insert(path.to_string(), target);
    }
    Ok(labels)
}

fn run() -> Result<()> {
    let mut input = String::new();
    std::io::stdin().read_to_string(&mut input)?;
    let labels = read_labels(&input)?;

    let model = Model::new(git::toplevel()?)?;
    let diff = compute_diff(&model, &labels);

    let stdout = std::io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    for r in &diff.renames {
        let dir = labels.get(&r.src).map(String::as_str).unwrap_or("");
        writeln!(
            out,
            "R\0{}\0subsystem={dir}\0{}\0{}\0{}",
            if r.ok { '1' } else { '0' },
            r.src,
            r.dst,
            r.reason,
        )?;
    }
    if !diff.moved.is_empty() {
        for referrer in ["Makefile", "meson.build"] {
            let content = patch_content(&model, referrer, &diff.moved)?;
            writeln!(out, "P\0{referrer}\0{}", content.len())?;
            out.write_all(content.as_bytes())?;
        }
    }
    out.flush()?;
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            if let Some(io) = e.root_cause().downcast_ref::<std::io::Error>() {
                if io.kind() == std::io::ErrorKind::BrokenPipe {
                    return ExitCode::SUCCESS;
                }
            }
            eprintln!("{e:#}");
            ExitCode::FAILURE
        }
    }
}
