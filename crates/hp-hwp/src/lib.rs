//! HWP 5.x (MS-CFB) codec for HangulPad.
//!
//! Phase 1 ships a **read stub**: detect CFB containers and refuse with a clear
//! error. Full record parsing → IR lands in a later phase. HWP write is deferred.

#![deny(missing_docs)]

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use thiserror::Error;

/// Compound File Binary (OLE2) magic used by HWP 5.x.
pub const CFB_MAGIC: [u8; 8] = [0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1];

/// HWP codec errors.
#[derive(Debug, Error)]
pub enum Error {
    /// I/O failure.
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    /// Path context.
    #[error("{context}: {path}")]
    Path {
        /// What failed.
        context: &'static str,
        /// Path involved.
        path: PathBuf,
        /// Underlying error.
        #[source]
        source: std::io::Error,
    },

    /// File is not a CFB/HWP container.
    #[error("not an HWP CFB file (missing OLE2 magic): {0}")]
    NotCfb(PathBuf),

    /// Recognized as HWP CFB, but reader not implemented yet.
    #[error(
        "HWP read is not implemented yet (Phase 1 stub). \
         Detected CFB container at {0}. Convert to HWPX or wait for hp-hwp Phase 2."
    )]
    ReadNotImplemented(PathBuf),

    /// HWP write is explicitly out of scope for early phases.
    #[error("HWP write is not supported yet (default save format is HWPX)")]
    WriteNotSupported,
}

/// HWP codec result.
pub type Result<T> = std::result::Result<T, Error>;

/// Returns `true` if `path` begins with the CFB/OLE2 magic (likely HWP 5.x).
pub fn looks_like_hwp(path: impl AsRef<Path>) -> Result<bool> {
    let path = path.as_ref();
    let mut file = File::open(path).map_err(|source| Error::Path {
        context: "open for HWP probe",
        path: path.to_path_buf(),
        source,
    })?;
    let mut magic = [0u8; 8];
    match file.read_exact(&mut magic) {
        Ok(()) => Ok(magic == CFB_MAGIC),
        Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => Ok(false),
        Err(e) => Err(e.into()),
    }
}

/// Attempt to read HWP → IR. Phase 1: detect CFB and return [`Error::ReadNotImplemented`].
pub fn read_path(path: impl AsRef<Path>) -> Result<hp_ir::Document> {
    let path = path.as_ref();
    if !looks_like_hwp(path)? {
        return Err(Error::NotCfb(path.to_path_buf()));
    }
    // Touch file length so empty CFB-looking stubs still exercise I/O.
    let mut file = File::open(path).map_err(|source| Error::Path {
        context: "open HWP",
        path: path.to_path_buf(),
        source,
    })?;
    let _len = file.seek(SeekFrom::End(0))?;
    Err(Error::ReadNotImplemented(path.to_path_buf()))
}

/// HWP write — deferred (use HWPX via `hp-hwpx`).
pub fn write_path(_doc: &hp_ir::Document, _path: impl AsRef<Path>) -> Result<()> {
    Err(Error::WriteNotSupported)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn rejects_non_cfb() {
        let dir = std::env::temp_dir().join("hp-hwp-non-cfb");
        let _ = std::fs::create_dir_all(&dir);
        let path = dir.join("plain.txt");
        std::fs::write(&path, b"hello").unwrap();
        assert!(!looks_like_hwp(&path).unwrap());
        let err = read_path(&path).unwrap_err();
        assert!(matches!(err, Error::NotCfb(_)));
    }

    #[test]
    fn stub_on_cfb_magic() {
        let dir = std::env::temp_dir().join("hp-hwp-cfb");
        let _ = std::fs::create_dir_all(&dir);
        let path = dir.join("fake.hwp");
        let mut f = File::create(&path).unwrap();
        f.write_all(&CFB_MAGIC).unwrap();
        f.write_all(&[0u8; 64]).unwrap();
        assert!(looks_like_hwp(&path).unwrap());
        let err = read_path(&path).unwrap_err();
        assert!(matches!(err, Error::ReadNotImplemented(_)));
    }

    #[test]
    fn write_unsupported() {
        let err = write_path(&hp_ir::Document::new(), "out.hwp").unwrap_err();
        assert!(matches!(err, Error::WriteNotSupported));
    }
}
