//! Thin git subprocess helpers.

use anyhow::{bail, Context, Result};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// The worktree root (`git rev-parse --show-toplevel`).
pub fn toplevel() -> Result<PathBuf> {
    let out = Command::new("git")
        .args(["rev-parse", "--show-toplevel"])
        .output()
        .context("running git rev-parse --show-toplevel")?;
    if !out.status.success() {
        bail!(
            "git rev-parse --show-toplevel: {}",
            String::from_utf8_lossy(&out.stderr).trim()
        );
    }
    Ok(PathBuf::from(String::from_utf8_lossy(&out.stdout).trim()))
}

/// Run `git -C top <args>` and return stdout, failing on an exit code not
/// in `allow` (pass `&[0, 1]` where no-match is expected, as for grep).
pub fn run(top: &Path, args: &[&str], allow: &[i32]) -> Result<String> {
    let out = Command::new("git")
        .arg("-C")
        .arg(top)
        .args(args)
        .output()
        .with_context(|| format!("running git {}", args.join(" ")))?;
    let code = out.status.code().unwrap_or(-1);
    if !allow.contains(&code) {
        bail!(
            "git {}: {}",
            args.join(" "),
            String::from_utf8_lossy(&out.stderr).trim()
        );
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

/// Run `git -C top <args>` with `input` on stdin, returning stdout. The
/// payload is small (a NUL-joined file list), so a single write cannot
/// deadlock against the child's output.
pub fn run_with_stdin(top: &Path, args: &[&str], input: &[u8]) -> Result<String> {
    let mut child = Command::new("git")
        .arg("-C")
        .arg(top)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .with_context(|| format!("spawning git {}", args.join(" ")))?;
    child
        .stdin
        .take()
        .expect("stdin was piped")
        .write_all(input)
        .with_context(|| format!("writing stdin to git {}", args.join(" ")))?;
    let out = child
        .wait_with_output()
        .with_context(|| format!("running git {}", args.join(" ")))?;
    if !out.status.success() {
        bail!(
            "git {}: {}",
            args.join(" "),
            String::from_utf8_lossy(&out.stderr).trim()
        );
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}
