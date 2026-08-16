//! `os.path`-style path and string helpers, kept borrow-friendly.

/// The final path component.
pub fn basename(p: &str) -> &str {
    p.rsplit('/').next().unwrap_or(p)
}

/// Everything before the last '/', or "" for a bare name.
pub fn dirname(p: &str) -> &str {
    match p.rfind('/') {
        Some(i) => &p[..i],
        None => "",
    }
}

/// Join two path fragments (b is always relative here).
pub fn join(a: &str, b: &str) -> String {
    if a.is_empty() {
        b.to_string()
    } else {
        format!("{a}/{b}")
    }
}

/// `os.path.normpath`: collapse '.', '..', and repeated separators.
pub fn normpath(p: &str) -> String {
    if p.is_empty() {
        return ".".to_string();
    }
    let is_abs = p.starts_with('/');
    let mut out: Vec<&str> = Vec::new();
    for comp in p.split('/') {
        match comp {
            "" | "." => {}
            ".." => match out.last() {
                Some(&last) if last != ".." => {
                    out.pop();
                }
                Some(_) => out.push(".."),
                None if !is_abs => out.push(".."),
                None => {}
            },
            c => out.push(c),
        }
    }
    let joined = out.join("/");
    if is_abs {
        format!("/{joined}")
    } else if joined.is_empty() {
        ".".to_string()
    } else {
        joined
    }
}

/// The filename without its `.c` or `.h` suffix.
pub fn stem(path: &str) -> &str {
    let base = basename(path);
    if base.ends_with(".c") || base.ends_with(".h") {
        &base[..base.len() - 2]
    } else {
        base
    }
}

/// Lowercase, then strip a trailing `.c` or `.h`.
pub fn norm(p: &str) -> String {
    let p = p.to_lowercase();
    match p.strip_suffix(".c").or_else(|| p.strip_suffix(".h")) {
        Some(s) => s.to_string(),
        None => p,
    }
}
