//! OWPML / HWPX XML namespaces used by HangulPad.

/// Paragraph namespace (`hp`).
pub const HP: &str = "http://www.hancom.co.kr/hwpml/2011/paragraph";
/// Section namespace (`hs`).
pub const HS: &str = "http://www.hancom.co.kr/hwpml/2011/section";
/// Head namespace (`hh`).
pub const HH: &str = "http://www.hancom.co.kr/hwpml/2011/head";
/// Core namespace (`hc`).
pub const HC: &str = "http://www.hancom.co.kr/hwpml/2011/core";
/// Version namespace (`hv`) — Hancom `version.xml` commonly uses this URI.
pub const HV: &str = "http://www.hancom.co.kr/hwpml/2011/version";
/// OPF package (`opf`).
pub const OPF: &str = "http://www.idpf.org/2007/opf/";
/// Dublin Core (`dc`).
pub const DC: &str = "http://purl.org/dc/elements/1.1/";

/// Clark notation helper: `{uri}local`.
pub fn clark(uri: &str, local: &str) -> String {
    format!("{{{uri}}}{local}")
}
