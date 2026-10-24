//! HWPX → IR.

use std::fs::File;
use std::io::{BufReader, Read};
use std::path::Path;

use hp_ir::{Align, Document, Meta, Paragraph, Run, Section};
use quick_xml::events::Event;
use quick_xml::Reader;
use zip::ZipArchive;

use crate::error::{Error, Result};
use crate::package::{entries, is_hwpx_mimetype};
use crate::xml_ns::{self, clark};

/// Read an HWPX file from `path` into IR.
pub fn read_path(path: impl AsRef<Path>) -> Result<Document> {
    let path = path.as_ref();
    let file = File::open(path).map_err(|source| Error::Path {
        context: "open HWPX",
        path: path.to_path_buf(),
        source,
    })?;
    read_archive(ZipArchive::new(BufReader::new(file))?)
}

fn read_archive<R: Read + std::io::Seek>(mut archive: ZipArchive<R>) -> Result<Document> {
    let mimetype = read_required_string(&mut archive, entries::MIMETYPE)?;
    if !is_hwpx_mimetype(&mimetype) {
        return Err(Error::InvalidMimetype(mimetype));
    }

    let section_names = discover_sections(&mut archive)?;
    if section_names.is_empty() {
        return Err(Error::MissingEntry(entries::SECTION0.into()));
    }

    let meta = parse_meta_from_hpf(&mut archive).unwrap_or_default();

    let mut sections = Vec::with_capacity(section_names.len());
    for name in section_names {
        let xml = read_required_string(&mut archive, &name)?;
        let paragraphs = parse_section_paragraphs(&xml)?;
        sections.push(Section {
            page: Default::default(),
            paragraphs,
        });
    }

    Ok(Document {
        meta,
        sections,
        bins: Vec::new(),
    })
}

fn read_required_string<R: Read + std::io::Seek>(
    archive: &mut ZipArchive<R>,
    name: &str,
) -> Result<String> {
    let mut entry = archive
        .by_name(name)
        .map_err(|_| Error::MissingEntry(name.into()))?;
    let mut buf = String::new();
    entry.read_to_string(&mut buf)?;
    Ok(buf)
}

fn discover_sections<R: Read + std::io::Seek>(archive: &mut ZipArchive<R>) -> Result<Vec<String>> {
    if let Ok(hpf) = read_required_string(archive, entries::CONTENT_HPF) {
        let from_spine = parse_section_hrefs_from_hpf(&hpf)?;
        if !from_spine.is_empty() {
            return Ok(from_spine);
        }
    }

    let mut names: Vec<String> = Vec::new();
    for i in 0..archive.len() {
        let entry = archive.by_index(i)?;
        let name = entry.name().to_string();
        if name.starts_with("Contents/section") && name.ends_with(".xml") {
            names.push(name);
        }
    }
    names.sort();
    Ok(names)
}

fn parse_section_hrefs_from_hpf(xml: &str) -> Result<Vec<String>> {
    let mut reader = Reader::from_str(xml);
    reader.config_mut().trim_text(true);
    let mut buf = Vec::new();
    let mut item_href: std::collections::HashMap<String, String> = std::collections::HashMap::new();
    let mut spine_refs: Vec<String> = Vec::new();
    let mut in_spine = false;

    let item_tag = clark(xml_ns::OPF, "item");
    let itemref_tag = clark(xml_ns::OPF, "itemref");
    let spine_tag = clark(xml_ns::OPF, "spine");

    loop {
        match reader.read_event_into(&mut buf) {
            Ok(Event::Start(e)) | Ok(Event::Empty(e)) => {
                let name = String::from_utf8_lossy(e.name().as_ref()).into_owned();
                let local = local_name(&name);

                if name == spine_tag || local == "spine" {
                    in_spine = true;
                }
                if name == item_tag || local == "item" {
                    let mut id = None;
                    let mut href = None;
                    for attr in e.attributes().flatten() {
                        let key_owned = String::from_utf8_lossy(attr.key.as_ref()).into_owned();
                        let key = local_name(&key_owned);
                        let val = attr
                            .unescape_value()
                            .map(|v| v.into_owned())
                            .unwrap_or_default();
                        match key {
                            "id" => id = Some(val),
                            "href" => href = Some(val),
                            _ => {}
                        }
                    }
                    if let (Some(id), Some(href)) = (id, href) {
                        item_href.insert(id, normalize_contents_href(&href));
                    }
                }
                if in_spine && (name == itemref_tag || local == "itemref") {
                    for attr in e.attributes().flatten() {
                        let key_owned = String::from_utf8_lossy(attr.key.as_ref()).into_owned();
                        let key = local_name(&key_owned);
                        if key == "idref" {
                            if let Ok(val) = attr.unescape_value() {
                                spine_refs.push(val.into_owned());
                            }
                        }
                    }
                }
            }
            Ok(Event::End(e)) => {
                let name = String::from_utf8_lossy(e.name().as_ref()).into_owned();
                if name == spine_tag || local_name(&name) == "spine" {
                    in_spine = false;
                }
            }
            Ok(Event::Eof) => break,
            Err(e) => return Err(Error::Xml(e.to_string())),
            _ => {}
        }
        buf.clear();
    }

    let mut sections = Vec::new();
    for idref in spine_refs {
        if let Some(href) = item_href.get(&idref) {
            let base = href.rsplit('/').next().unwrap_or(href);
            if base.starts_with("section") && base.ends_with(".xml") {
                sections.push(href.clone());
            }
        }
    }

    if sections.is_empty() {
        let mut candidates: Vec<_> = item_href
            .values()
            .filter(|h| {
                let base = h.rsplit('/').next().unwrap_or(h);
                base.starts_with("section") && base.ends_with(".xml")
            })
            .cloned()
            .collect();
        candidates.sort();
        sections = candidates;
    }

    Ok(sections)
}

fn normalize_contents_href(href: &str) -> String {
    let href = href.trim_start_matches("./");
    if href.starts_with("Contents/") || href.contains('/') {
        href.to_string()
    } else {
        format!("Contents/{href}")
    }
}

fn parse_meta_from_hpf<R: Read + std::io::Seek>(archive: &mut ZipArchive<R>) -> Result<Meta> {
    let xml = read_required_string(archive, entries::CONTENT_HPF)?;
    let mut reader = Reader::from_str(&xml);
    reader.config_mut().trim_text(true);
    let mut buf = Vec::new();
    let mut meta = Meta {
        title: String::new(),
        language: "ko".into(),
    };
    let mut current: Option<&'static str> = None;

    loop {
        match reader.read_event_into(&mut buf) {
            Ok(Event::Start(e)) => {
                let name_owned = String::from_utf8_lossy(e.name().as_ref()).into_owned();
                let local = local_name(&name_owned);
                current = match local {
                    "title" => Some("title"),
                    "language" => Some("language"),
                    _ => None,
                };
            }
            Ok(Event::Text(t)) => {
                if let Some(field) = current {
                    let text = t.unescape().map(|s| s.into_owned()).unwrap_or_default();
                    match field {
                        "title" => meta.title = text,
                        "language" if !text.is_empty() => meta.language = text,
                        _ => {}
                    }
                }
            }
            Ok(Event::End(_)) => current = None,
            Ok(Event::Eof) => break,
            Err(e) => return Err(Error::Xml(e.to_string())),
            _ => {}
        }
        buf.clear();
    }

    Ok(meta)
}

fn parse_section_paragraphs(xml: &str) -> Result<Vec<Paragraph>> {
    let mut reader = Reader::from_str(xml);
    reader.config_mut().trim_text(false);
    let mut buf = Vec::new();

    let mut paragraphs: Vec<Paragraph> = Vec::new();
    let mut current_para: Option<Paragraph> = None;
    let mut in_t = false;
    let mut t_buf = String::new();

    let p_names = [clark(xml_ns::HP, "p"), "hp:p".into(), "p".into()];
    let t_names = [clark(xml_ns::HP, "t"), "hp:t".into(), "t".into()];
    let line_break_names = [
        clark(xml_ns::HP, "lineBreak"),
        "hp:lineBreak".into(),
        "lineBreak".into(),
    ];

    loop {
        match reader.read_event_into(&mut buf) {
            Ok(Event::Start(ref e)) => {
                let name = String::from_utf8_lossy(e.name().as_ref()).into_owned();
                if name_matches(&name, &p_names) {
                    current_para = Some(Paragraph {
                        align: Align::Left,
                        runs: Vec::new(),
                    });
                } else if name_matches(&name, &t_names) {
                    in_t = true;
                    t_buf.clear();
                } else if name_matches(&name, &line_break_names) {
                    if let Some(para) = current_para.as_mut() {
                        para.runs.push(Run::Break);
                    }
                }
            }
            Ok(Event::Empty(ref e)) => {
                let name = String::from_utf8_lossy(e.name().as_ref()).into_owned();
                if name_matches(&name, &line_break_names) {
                    if let Some(para) = current_para.as_mut() {
                        para.runs.push(Run::Break);
                    }
                } else if name_matches(&name, &t_names) {
                    if let Some(para) = current_para.as_mut() {
                        para.runs.push(Run::text(""));
                    }
                }
            }
            Ok(Event::Text(t)) => {
                if in_t {
                    let text = t.unescape().map(|s| s.into_owned()).unwrap_or_default();
                    t_buf.push_str(&text);
                }
            }
            Ok(Event::CData(t)) => {
                if in_t {
                    t_buf.push_str(&String::from_utf8_lossy(&t.into_inner()));
                }
            }
            Ok(Event::End(ref e)) => {
                let name = String::from_utf8_lossy(e.name().as_ref()).into_owned();
                if name_matches(&name, &t_names) {
                    in_t = false;
                    if let Some(para) = current_para.as_mut() {
                        para.runs.push(Run::text(std::mem::take(&mut t_buf)));
                    }
                } else if name_matches(&name, &p_names) {
                    if let Some(para) = current_para.take() {
                        // Skip structural-only paragraphs (e.g. secPr with no text).
                        let has_text = para
                            .runs
                            .iter()
                            .any(|r| matches!(r, Run::Text { .. } | Run::Break));
                        if has_text {
                            paragraphs.push(para);
                        }
                    }
                }
            }
            Ok(Event::Eof) => break,
            Err(e) => return Err(Error::Xml(e.to_string())),
            _ => {}
        }
        buf.clear();
    }

    Ok(paragraphs)
}

fn name_matches(name: &str, candidates: &[String]) -> bool {
    let local = local_name(name);
    candidates
        .iter()
        .any(|c| c == name || local_name(c) == local)
}

fn local_name(name: &str) -> &str {
    if let Some(rest) = name.strip_prefix('{') {
        rest.split_once('}').map(|(_, local)| local).unwrap_or(name)
    } else if let Some((_, local)) = name.split_once(':') {
        local
    } else {
        name
    }
}
