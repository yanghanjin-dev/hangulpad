//! IR → HWPX.

use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;

use hp_ir::{Document, Paragraph, Run};
use zip::write::SimpleFileOptions;
use zip::{CompressionMethod, ZipWriter};

use crate::error::{Error, Result};
use crate::package::{entries, MIMETYPE};
use crate::xml_ns;

/// Write IR as a minimal valid HWPX package.
pub fn write_path(doc: &Document, path: impl AsRef<Path>) -> Result<()> {
    let path = path.as_ref();
    let file = File::create(path).map_err(|source| Error::Path {
        context: "create HWPX",
        path: path.to_path_buf(),
        source,
    })?;
    write_archive(doc, BufWriter::new(file))
}

fn write_archive<W: Write + std::io::Seek>(doc: &Document, writer: W) -> Result<()> {
    let mut zip = ZipWriter::new(writer);

    let stored = SimpleFileOptions::default().compression_method(CompressionMethod::Stored);
    let deflated = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);

    // mimetype MUST be first and STORED (OWPML / EPUB-style packaging rule).
    zip.start_file(entries::MIMETYPE, stored)?;
    zip.write_all(MIMETYPE.as_bytes())?;

    zip.start_file(entries::VERSION, deflated)?;
    zip.write_all(version_xml().as_bytes())?;

    zip.start_file(entries::CONTAINER, deflated)?;
    zip.write_all(container_xml().as_bytes())?;

    zip.start_file(entries::CONTENT_HPF, deflated)?;
    zip.write_all(content_hpf(doc).as_bytes())?;

    zip.start_file(entries::HEADER, deflated)?;
    zip.write_all(header_xml(doc).as_bytes())?;

    let sections = if doc.sections.is_empty() {
        vec![Default::default()]
    } else {
        doc.sections.clone()
    };

    for (idx, section) in sections.iter().enumerate() {
        let name = format!("Contents/section{idx}.xml");
        zip.start_file(&name, deflated)?;
        zip.write_all(section_xml(section).as_bytes())?;
    }

    for bin in &doc.bins {
        zip.start_file(&bin.path, stored)?;
        zip.write_all(&bin.data)?;
    }

    zip.finish()?;
    Ok(())
}

fn xml_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for ch in s.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&apos;"),
            _ => out.push(ch),
        }
    }
    out
}

fn version_xml() -> String {
    let mut s = String::from(
        r#"<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<hv:HCFVersion xmlns:hv=""#,
    );
    s.push_str(xml_ns::HV);
    s.push_str(
        r#"" tagetApplication="WORDPROCESSOR" major="5" minor="1" micro="0" buildNumber="0" os="0" xmlVersion="1.5" application="HangulPad" appVersion="0.1.0"/>
"#,
    );
    s
}

fn container_xml() -> String {
    r#"<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<ocf:container xmlns:ocf="urn:oasis:names:tc:opendocument:xmlns:container">
  <ocf:rootfiles>
    <ocf:rootfile full-path="Contents/content.hpf" media-type="application/hwpml-package+xml"/>
  </ocf:rootfiles>
</ocf:container>
"#
    .into()
}

fn content_hpf(doc: &Document) -> String {
    let title = xml_escape(if doc.meta.title.is_empty() {
        "Untitled"
    } else {
        &doc.meta.title
    });
    let lang = xml_escape(if doc.meta.language.is_empty() {
        "ko"
    } else {
        &doc.meta.language
    });
    let section_count = doc.sections.len().max(1);

    let mut manifest = String::from(
        r#"    <opf:item id="header" href="header.xml" media-type="application/xml"/>
"#,
    );
    let mut spine = String::new();
    for i in 0..section_count {
        manifest.push_str(&format!(
            "    <opf:item id=\"section{i}\" href=\"section{i}.xml\" media-type=\"application/xml\"/>\n"
        ));
        spine.push_str(&format!("    <opf:itemref idref=\"section{i}\"/>\n"));
    }

    let mut out = String::new();
    out.push_str(
        r#"<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<opf:package version="2.0" unique-identifier="uid" xmlns:opf=""#,
    );
    out.push_str(xml_ns::OPF);
    out.push_str(r#"" xmlns:dc=""#);
    out.push_str(xml_ns::DC);
    out.push_str(
        r#"" xmlns:hpf="http://www.hancom.co.kr/schema/2011/hpf">
  <opf:metadata>
    <dc:title>"#,
    );
    out.push_str(&title);
    out.push_str("</dc:title>\n    <dc:language>");
    out.push_str(&lang);
    out.push_str(
        r#"</dc:language>
    <dc:identifier id="uid">hangulpad</dc:identifier>
    <opf:meta name="creator" content="HangulPad"/>
  </opf:metadata>
  <opf:manifest>
"#,
    );
    out.push_str(&manifest);
    out.push_str("  </opf:manifest>\n  <opf:spine>\n");
    out.push_str(&spine);
    out.push_str("  </opf:spine>\n</opf:package>\n");
    out
}

fn header_xml(doc: &Document) -> String {
    let sec_cnt = doc.sections.len().max(1);
    // Static OWPML header skeleton (avoid format! so `#` / `0.1 mm` stay literal).
    let mut out = String::new();
    out.push_str(
        r#"<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<hh:head version="1.5" secCnt=""#,
    );
    out.push_str(&sec_cnt.to_string());
    out.push_str(r#"" xmlns:hh=""#);
    out.push_str(xml_ns::HH);
    out.push_str(r#"" xmlns:hp=""#);
    out.push_str(xml_ns::HP);
    out.push_str(r#"" xmlns:hc=""#);
    out.push_str(xml_ns::HC);
    out.push_str(
        r##"">
  <hh:beginNum page="1" footnote="1" endnote="1" pic="1" tbl="1" equation="1"/>
  <hh:refList>
    <hh:fontfaces itemCnt="1">
      <hh:fontface lang="HANGUL" fontCnt="1">
        <hh:font id="0" face="맑은 고딕" type="TTF" isEmbedded="0"/>
      </hh:fontface>
      <hh:fontface lang="LATIN" fontCnt="1">
        <hh:font id="0" face="맑은 고딕" type="TTF" isEmbedded="0"/>
      </hh:fontface>
      <hh:fontface lang="HANJA" fontCnt="1">
        <hh:font id="0" face="맑은 고딕" type="TTF" isEmbedded="0"/>
      </hh:fontface>
      <hh:fontface lang="JAPANESE" fontCnt="1">
        <hh:font id="0" face="맑은 고딕" type="TTF" isEmbedded="0"/>
      </hh:fontface>
      <hh:fontface lang="OTHER" fontCnt="1">
        <hh:font id="0" face="맑은 고딕" type="TTF" isEmbedded="0"/>
      </hh:fontface>
      <hh:fontface lang="SYMBOL" fontCnt="1">
        <hh:font id="0" face="맑은 고딕" type="TTF" isEmbedded="0"/>
      </hh:fontface>
      <hh:fontface lang="USER" fontCnt="1">
        <hh:font id="0" face="맑은 고딕" type="TTF" isEmbedded="0"/>
      </hh:fontface>
    </hh:fontfaces>
    <hh:borderFills itemCnt="1">
      <hh:borderFill id="0" threeD="0" shadow="0" centerLine="NONE" breakCellSeparateLine="0">
        <hh:slash type="NONE" crooked="0" isCounter="0"/>
        <hh:backSlash type="NONE" crooked="0" isCounter="0"/>
        <hh:leftBorder type="NONE" width="0.1 mm" color="#000000"/>
        <hh:rightBorder type="NONE" width="0.1 mm" color="#000000"/>
        <hh:topBorder type="NONE" width="0.1 mm" color="#000000"/>
        <hh:bottomBorder type="NONE" width="0.1 mm" color="#000000"/>
        <hh:diagonal type="NONE" width="0.1 mm" color="#000000"/>
      </hh:borderFill>
    </hh:borderFills>
    <hh:charProperties itemCnt="1">
      <hh:charPr id="0" height="1000" textColor="#000000" shadeColor="none" useFontSpace="0" useKerning="0" symMark="NONE" borderFillIDRef="0">
        <hh:fontRef hangul="0" latin="0" hanja="0" japanese="0" other="0" symbol="0" user="0"/>
        <hh:ratio hangul="100" latin="100" hanja="100" japanese="100" other="100" symbol="100" user="100"/>
        <hh:spacing hangul="0" latin="0" hanja="0" japanese="0" other="0" symbol="0" user="0"/>
        <hh:relSz hangul="100" latin="100" hanja="100" japanese="100" other="100" symbol="100" user="100"/>
        <hh:offset hangul="0" latin="0" hanja="0" japanese="0" other="0" symbol="0" user="0"/>
      </hh:charPr>
    </hh:charProperties>
    <hh:tabProperties itemCnt="1">
      <hh:tabPr id="0" autoTabLeft="0" autoTabRight="0"/>
    </hh:tabProperties>
    <hh:paraProperties itemCnt="1">
      <hh:paraPr id="0" tabPrIDRef="0" condense="0" fontLineHeight="0" snapToGrid="1" suppressLineNumbers="0" checked="0">
        <hh:align horizontal="JUSTIFY" vertical="BASELINE"/>
        <hh:heading type="NONE" idRef="0"/>
        <hh:breakSetting breakLatinWord="KEEP_WORD" breakNonLatinWord="KEEP_WORD" widowOrphan="0" keepWithNext="0" keepLines="0" pageBreakBefore="0" lineWrap="BREAK"/>
        <hh:margin left="0" right="0" prev="0" next="0"/>
        <hh:lineSpacing type="PERCENT" value="160" unit="HWPUNIT"/>
        <hh:border borderFillIDRef="0" offsetLeft="0" offsetRight="0" offsetTop="0" offsetBottom="0" connect="0" ignoreMargin="0"/>
      </hh:paraPr>
    </hh:paraProperties>
    <hh:styles itemCnt="1">
      <hh:style id="0" type="PARA" name="바탕글" engName="Normal" paraPrIDRef="0" charPrIDRef="0"/>
    </hh:styles>
  </hh:refList>
</hh:head>
"##,
    );
    out
}

fn section_xml(section: &hp_ir::Section) -> String {
    let page = &section.page;
    let mut body = String::new();

    body.push_str("  <hp:p id=\"2147483648\" paraPrIDRef=\"0\" styleIDRef=\"0\" pageBreak=\"0\" columnBreak=\"0\" merged=\"0\">\n");
    body.push_str("    <hp:run charPrIDRef=\"0\">\n");
    body.push_str("      <hp:secPr id=\"\" textDirection=\"HORIZONTAL\" spaceColumns=\"1134\" tabStop=\"8000\" outlineShapeIDRef=\"1\" memoShapeIDRef=\"0\" textVerticalWidthHead=\"0\" masterPageCnt=\"0\">\n");
    body.push_str("        <hp:grid lineGrid=\"0\" charGrid=\"0\" type=\"NONE\"/>\n");
    body.push_str("        <hp:startNum pageStartsOn=\"BOTH\" page=\"0\" pic=\"0\" tbl=\"0\" equation=\"0\"/>\n");
    body.push_str("        <hp:visibility showLineNumber=\"0\" hideHeader=\"0\" hideFooter=\"0\" hideMasterPage=\"0\" hideBorder=\"0\" hideFill=\"0\" hidePageNumPos=\"0\" format=\"0\" hideEmptyLine=\"0\"/>\n");
    body.push_str(&format!(
        "        <hp:pagePr landscape=\"0\" width=\"{}\" height=\"{}\" gutterType=\"LEFT_RIGHT\">\n",
        page.width, page.height
    ));
    body.push_str(&format!(
        "          <hp:margin header=\"{}\" footer=\"{}\" gutter=\"0\" left=\"{}\" right=\"{}\" top=\"{}\" bottom=\"{}\"/>\n",
        page.margin_top,
        page.margin_bottom,
        page.margin_left,
        page.margin_right,
        page.margin_top,
        page.margin_bottom
    ));
    body.push_str("        </hp:pagePr>\n");
    body.push_str("      </hp:secPr>\n");
    body.push_str("    </hp:run>\n");
    body.push_str("  </hp:p>\n");

    if section.paragraphs.is_empty() {
        body.push_str(&paragraph_xml(&Paragraph::empty(), 0));
    } else {
        for (i, para) in section.paragraphs.iter().enumerate() {
            body.push_str(&paragraph_xml(para, i as u32));
        }
    }

    let mut out = String::new();
    out.push_str(
        r#"<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<hs:sec xmlns:hs=""#,
    );
    out.push_str(xml_ns::HS);
    out.push_str(r#"" xmlns:hp=""#);
    out.push_str(xml_ns::HP);
    out.push_str(r#"" xmlns:hc=""#);
    out.push_str(xml_ns::HC);
    out.push_str("\">\n");
    out.push_str(&body);
    out.push_str("</hs:sec>\n");
    out
}

fn paragraph_xml(para: &Paragraph, id: u32) -> String {
    let mut runs_xml = String::new();
    if para.runs.is_empty() {
        runs_xml.push_str("    <hp:run charPrIDRef=\"0\">\n      <hp:t></hp:t>\n    </hp:run>\n");
    } else {
        runs_xml.push_str("    <hp:run charPrIDRef=\"0\">\n");
        for run in &para.runs {
            match run {
                Run::Text { text, .. } => {
                    runs_xml.push_str("      <hp:t>");
                    runs_xml.push_str(&xml_escape(text));
                    runs_xml.push_str("</hp:t>\n");
                }
                Run::Break => {
                    runs_xml.push_str("      <hp:lineBreak/>\n");
                }
            }
        }
        runs_xml.push_str("    </hp:run>\n");
    }

    format!(
        "  <hp:p id=\"{id}\" paraPrIDRef=\"0\" styleIDRef=\"0\" pageBreak=\"0\" columnBreak=\"0\" merged=\"0\">\n{runs_xml}  </hp:p>\n"
    )
}
