//! HWPX ZIP package constants and helpers.

use std::fs::File;
use std::io::{BufReader, Read};
use std::path::Path;

use zip::ZipArchive;

use crate::error::Result;

/// Canonical HWPX mimetype payload (must be first ZIP entry, STORED).
pub const MIMETYPE: &str = "application/hwp+zip";

/// Returns true if `value` is a recognized HWPX mimetype.
pub fn is_hwpx_mimetype(value: &str) -> bool {
    value.trim() == MIMETYPE
}

/// Returns `true` if `path` is a ZIP with an `application/hwp+zip` mimetype entry.
pub fn looks_like_hwpx(path: impl AsRef<Path>) -> Result<bool> {
    let file = File::open(path.as_ref())?;
    let mut archive = match ZipArchive::new(BufReader::new(file)) {
        Ok(a) => a,
        Err(_) => return Ok(false),
    };
    let mut entry = match archive.by_name(entries::MIMETYPE) {
        Ok(e) => e,
        Err(_) => return Ok(false),
    };
    let mut s = String::new();
    entry.read_to_string(&mut s)?;
    Ok(is_hwpx_mimetype(&s))
}

/// Entry names used by a minimal HangulPad HWPX package.
pub mod entries {
    /// `mimetype`
    pub const MIMETYPE: &str = "mimetype";
    /// `version.xml`
    pub const VERSION: &str = "version.xml";
    /// `Contents/content.hpf`
    pub const CONTENT_HPF: &str = "Contents/content.hpf";
    /// `Contents/header.xml`
    pub const HEADER: &str = "Contents/header.xml";
    /// `Contents/section0.xml`
    pub const SECTION0: &str = "Contents/section0.xml";
    /// `META-INF/container.xml` (optional but written for compatibility).
    pub const CONTAINER: &str = "META-INF/container.xml";
}
