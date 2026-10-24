//! HangulPad internal document model (IR).
//!
//! Codecs (`hp-hwpx`, `hp-hwp`) convert to/from this model. The UI and CLI
//! should only see IR — never ZIP/CFB details.

#![deny(missing_docs)]

/// Document-level metadata.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Meta {
    /// Document title (Dublin Core / HWPX metadata).
    pub title: String,
    /// Primary language tag, e.g. `ko`.
    pub language: String,
}

/// Character run formatting (MVP subset).
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct CharFormat {
    /// Font face name (Hangul face for MVP).
    pub font: Option<String>,
    /// Height in HWPUNIT (1 pt ≈ 100 HWPUNIT).
    pub height_hwpunit: Option<u32>,
    /// Bold.
    pub bold: bool,
    /// Italic.
    pub italic: bool,
    /// Underline.
    pub underline: bool,
    /// Text color as `#RRGGBB`, if known.
    pub text_color: Option<String>,
}

/// Inline content inside a paragraph.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Run {
    /// Plain text span.
    Text {
        /// UTF-8 text.
        text: String,
        /// Character formatting (defaults when unset).
        format: CharFormat,
    },
    /// Soft/hard line break (MVP: treated as newline on write).
    Break,
}

impl Run {
    /// Convenience constructor for unformatted text.
    pub fn text(text: impl Into<String>) -> Self {
        Self::Text {
            text: text.into(),
            format: CharFormat::default(),
        }
    }
}

/// Paragraph alignment (MVP).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Align {
    /// Left.
    #[default]
    Left,
    /// Center.
    Center,
    /// Right.
    Right,
    /// Justify / both.
    Both,
}

/// A paragraph of runs.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Paragraph {
    /// Paragraph alignment.
    pub align: Align,
    /// Inline runs.
    pub runs: Vec<Run>,
}

impl Paragraph {
    /// Empty paragraph.
    pub fn empty() -> Self {
        Self::default()
    }

    /// Single-text paragraph.
    pub fn from_text(text: impl Into<String>) -> Self {
        Self {
            align: Align::Left,
            runs: vec![Run::text(text)],
        }
    }

    /// Concatenate text runs (Break → `\n`).
    pub fn plain_text(&self) -> String {
        let mut out = String::new();
        for run in &self.runs {
            match run {
                Run::Text { text, .. } => out.push_str(text),
                Run::Break => out.push('\n'),
            }
        }
        out
    }
}

/// Page size and margins in HWPUNIT (1 mm ≈ 283.465 HWPUNIT; A4 ≈ 59528×84188).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PageSetup {
    /// Page width.
    pub width: u32,
    /// Page height.
    pub height: u32,
    /// Left margin.
    pub margin_left: u32,
    /// Right margin.
    pub margin_right: u32,
    /// Top margin.
    pub margin_top: u32,
    /// Bottom margin.
    pub margin_bottom: u32,
}

impl Default for PageSetup {
    fn default() -> Self {
        // A4 with ~20mm margins (approximate HWPUNIT).
        Self {
            width: 59528,
            height: 84188,
            margin_left: 5669,
            margin_right: 5669,
            margin_top: 5669,
            margin_bottom: 4252,
        }
    }
}

/// Document section (maps to one `sectionN.xml` in HWPX).
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Section {
    /// Page geometry.
    pub page: PageSetup,
    /// Body paragraphs.
    pub paragraphs: Vec<Paragraph>,
}

/// Opaque binary payload (images, etc.) — reserved for later phases.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BinItem {
    /// Package-relative path, e.g. `BinData/image1.png`.
    pub path: String,
    /// Raw bytes.
    pub data: Vec<u8>,
}

/// Root document IR.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Document {
    /// Metadata.
    pub meta: Meta,
    /// Ordered sections (at least one for a valid save).
    pub sections: Vec<Section>,
    /// Binary store.
    pub bins: Vec<BinItem>,
}

impl Default for Document {
    fn default() -> Self {
        Self {
            meta: Meta {
                title: String::new(),
                language: "ko".into(),
            },
            sections: vec![Section::default()],
            bins: Vec::new(),
        }
    }
}

impl Document {
    /// New empty document with one blank section.
    pub fn new() -> Self {
        Self::default()
    }

    /// Document with a single paragraph of text.
    pub fn from_plain_text(text: impl Into<String>) -> Self {
        let mut doc = Self::new();
        doc.sections[0].paragraphs.push(Paragraph::from_text(text));
        doc
    }

    /// Flatten all paragraph plain text, joined by `\n`.
    pub fn plain_text(&self) -> String {
        self.sections
            .iter()
            .flat_map(|s| s.paragraphs.iter())
            .map(Paragraph::plain_text)
            .collect::<Vec<_>>()
            .join("\n")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plain_text_roundtrip_shape() {
        let doc = Document::from_plain_text("한글패드");
        assert_eq!(doc.plain_text(), "한글패드");
    }
}
