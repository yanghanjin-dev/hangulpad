//! Minimal valid HWPX roundtrip tests.

use std::fs;
use std::io::{Cursor, Read, Write};
use std::path::PathBuf;

use hp_hwpx::{read_path, write_path, MIMETYPE};
use hp_ir::{Document, Meta, Paragraph, Section};
use zip::write::SimpleFileOptions;
use zip::{CompressionMethod, ZipArchive, ZipWriter};

fn fixture(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures")
        .join(name)
}

fn temp_out(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join("hp-hwpx-tests");
    let _ = fs::create_dir_all(&dir);
    dir.join(name)
}

#[test]
fn write_then_read_preserves_korean_text() {
    let mut doc = Document::new();
    doc.meta = Meta {
        title: "테스트".into(),
        language: "ko".into(),
    };
    doc.sections[0]
        .paragraphs
        .push(Paragraph::from_text("한글패드 엔진"));
    doc.sections[0]
        .paragraphs
        .push(Paragraph::from_text("Phase 1 HWPX roundtrip"));

    let out = temp_out("write_read.hwpx");
    write_path(&doc, &out).expect("write");
    let loaded = read_path(&out).expect("read");

    assert_eq!(loaded.meta.title, "테스트");
    assert_eq!(loaded.meta.language, "ko");
    assert_eq!(loaded.plain_text(), "한글패드 엔진\nPhase 1 HWPX roundtrip");
}

#[test]
fn mimetype_is_first_and_stored() {
    let doc = Document::from_plain_text("stored-check");
    let out = temp_out("mimetype_order.hwpx");
    write_path(&doc, &out).expect("write");

    let file = fs::File::open(&out).unwrap();
    let mut zip = ZipArchive::new(file).unwrap();
    let first = zip.by_index(0).unwrap();
    assert_eq!(first.name(), "mimetype");
    assert_eq!(first.compression(), CompressionMethod::Stored);
    drop(first);
    let mut mt = zip.by_name("mimetype").unwrap();
    let mut s = String::new();
    mt.read_to_string(&mut s).unwrap();
    assert_eq!(s, MIMETYPE);
}

#[test]
fn fixture_simple_paragraph_roundtrip() {
    let input = fixture("simple_paragraph.hwpx");
    assert!(input.is_file(), "missing fixture at {}", input.display());
    let out = temp_out("fixture_roundtrip.hwpx");
    let doc = hp_hwpx::roundtrip(&input, &out).expect("roundtrip");
    assert!(
        doc.plain_text().contains("한글패드"),
        "unexpected text: {:?}",
        doc.plain_text()
    );
    let again = read_path(&out).expect("re-read");
    assert_eq!(again.plain_text(), doc.plain_text());
}

#[test]
fn empty_document_writes_valid_package() {
    let doc = Document {
        meta: Meta::default(),
        sections: vec![Section::default()],
        bins: vec![],
    };
    let out = temp_out("empty.hwpx");
    write_path(&doc, &out).expect("write empty");
    let loaded = read_path(&out).expect("read empty");
    assert_eq!(loaded.sections.len(), 1);
}

#[test]
fn checked_in_fixture_parses() {
    let input = fixture("simple_paragraph.hwpx");
    assert!(
        input.is_file(),
        "checked-in fixture missing: {}",
        input.display()
    );
    let doc = read_path(&input).expect("parse fixture");
    assert!(doc.plain_text().contains("한글패드"));
}

#[test]
fn handcrafted_minimal_zip_is_readable() {
    // Independently constructed package (not via write_path) to avoid tautology.
    let mut buf = Cursor::new(Vec::new());
    {
        let mut zip = ZipWriter::new(&mut buf);
        let stored = SimpleFileOptions::default().compression_method(CompressionMethod::Stored);
        let deflated = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);

        zip.start_file("mimetype", stored).unwrap();
        zip.write_all(MIMETYPE.as_bytes()).unwrap();

        zip.start_file("version.xml", deflated).unwrap();
        zip.write_all(
            br#"<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<hv:HCFVersion xmlns:hv="http://www.hancom.co.kr/hwpml/2011/version" tagetApplication="WORDPROCESSOR" major="5" minor="1" micro="0" buildNumber="0"/>
"#,
        )
        .unwrap();

        zip.start_file("Contents/content.hpf", deflated).unwrap();
        zip.write_all(
            br#"<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<opf:package version="2.0" unique-identifier="uid" xmlns:opf="http://www.idpf.org/2007/opf/" xmlns:dc="http://purl.org/dc/elements/1.1/">
  <opf:metadata>
    <dc:title>handcrafted</dc:title>
    <dc:language>ko</dc:language>
    <dc:identifier id="uid">hand</dc:identifier>
  </opf:metadata>
  <opf:manifest>
    <opf:item id="header" href="header.xml" media-type="application/xml"/>
    <opf:item id="section0" href="section0.xml" media-type="application/xml"/>
  </opf:manifest>
  <opf:spine>
    <opf:itemref idref="section0"/>
  </opf:spine>
</opf:package>
"#,
        )
        .unwrap();

        zip.start_file("Contents/header.xml", deflated).unwrap();
        zip.write_all(
            br#"<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<hh:head version="1.5" secCnt="1" xmlns:hh="http://www.hancom.co.kr/hwpml/2011/head"/>
"#,
        )
        .unwrap();

        zip.start_file("Contents/section0.xml", deflated).unwrap();
        zip.write_all(
            br#"<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<hs:sec xmlns:hs="http://www.hancom.co.kr/hwpml/2011/section" xmlns:hp="http://www.hancom.co.kr/hwpml/2011/paragraph">
  <hp:p id="0" paraPrIDRef="0" styleIDRef="0">
    <hp:run charPrIDRef="0">
      <hp:t>handcrafted hello</hp:t>
    </hp:run>
  </hp:p>
</hs:sec>
"#,
        )
        .unwrap();

        zip.finish().unwrap();
    }

    let path = temp_out("handcrafted.hwpx");
    fs::write(&path, buf.into_inner()).unwrap();
    let doc = read_path(&path).expect("read handcrafted");
    assert_eq!(doc.meta.title, "handcrafted");
    assert_eq!(doc.plain_text(), "handcrafted hello");
}
