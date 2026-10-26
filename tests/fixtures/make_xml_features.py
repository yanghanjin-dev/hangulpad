#!/usr/bin/env python3
"""Generate tests/fixtures/xml_features.hwpx — XML constructs the reader must handle.

Hangul itself rarely writes these, but they are valid XML and other tools do:
numeric character references, CDATA, comments inside text, another namespace
prefix, pretty-printed whitespace, text and a table in one run, a header control
(not body text), a nested table, and two sections listed in content.hpf.
Run:  python3 tests/fixtures/make_xml_features.py
"""
import os
import zipfile

NS = (' xmlns:hp="http://www.hancom.co.kr/hwpml/2011/paragraph"'
      ' xmlns:hs="http://www.hancom.co.kr/hwpml/2011/section"'
      ' xmlns:hc="http://www.hancom.co.kr/hwpml/2011/core"'
      ' xmlns:hh="http://www.hancom.co.kr/hwpml/2011/head"'
      ' xmlns:opf="http://www.idpf.org/2007/opf/"')

HEADER = """<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<hh:head%s version="1.5" secCnt="2">
  <hh:refList>
    <hh:fontfaces itemCnt="1"><hh:fontface lang="HANGUL" fontCnt="1">
      <hh:font id="0" face="맑은 고딕" type="TTF" isEmbedded="0"/></hh:fontface></hh:fontfaces>
    <hh:charProperties itemCnt="2">
      <hh:charPr id="0" height="1000" textColor="#000000"><hh:fontRef hangul="0"/></hh:charPr>
      <hh:charPr id="1" height="1600" textColor="#FF0000"><hh:fontRef hangul="0"/><hh:bold/></hh:charPr>
    </hh:charProperties>
    <hh:paraProperties itemCnt="1">
      <hh:paraPr id="0"><hh:align horizontal="LEFT"/></hh:paraPr>
    </hh:paraProperties>
  </hh:refList>
</hh:head>
""" % NS

# Section 0 uses the prefix "p" (not "hp") for the paragraph namespace.
SECTION0 = """<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<!-- pretty-printed on purpose -->
<hs:sec xmlns:hs="http://www.hancom.co.kr/hwpml/2011/section"
        xmlns:p="http://www.hancom.co.kr/hwpml/2011/paragraph">
  <p:p id="0" paraPrIDRef="0">
    <p:run charPrIDRef="0">
      <p:ctrl><p:header id="1"><p:subList><p:p><p:run><p:t>머리말은 본문이 아님</p:t></p:run></p:p></p:subList></p:header></p:ctrl>
      <p:t>숫자참조 &#xAC00;&#45208; 끝</p:t>
    </p:run>
  </p:p>
  <p:p id="0" paraPrIDRef='0'>
    <p:run charPrIDRef="1"><p:t><![CDATA[<raw> & text]]></p:t></p:run>
  </p:p>
  <p:p id="0" paraPrIDRef="0">
    <p:run charPrIDRef="0"><p:t>주<!-- comment -->석<p:fwSpace/>사이</p:t></p:run>
  </p:p>
  <p:p id="0" paraPrIDRef="0">
    <p:run charPrIDRef="0">
      <p:t>표 앞 글자</p:t>
      <p:tbl rowCnt="1" colCnt="2">
        <p:sz width="20000" height="2000"/>
        <p:pos treatAsChar="1" vertOffset="0" horzOffset="0"/>
        <p:tr>
          <p:tc borderFillIDRef="0">
            <p:subList><p:p paraPrIDRef="0"><p:run charPrIDRef="0"><p:t>A1</p:t></p:run></p:p></p:subList>
            <p:cellAddr colAddr="0" rowAddr="0"/><p:cellSpan colSpan="1" rowSpan="1"/>
            <p:cellSz width="10000" height="2000"/><p:cellMargin left="100" right="100" top="50" bottom="50"/>
          </p:tc>
          <p:tc borderFillIDRef="0">
            <p:subList><p:p paraPrIDRef="0"><p:run charPrIDRef="0">
              <p:tbl rowCnt="1" colCnt="1"><p:sz width="5000" height="1000"/>
                <p:tr><p:tc><p:subList><p:p><p:run><p:t>안쪽</p:t></p:run></p:p></p:subList>
                  <p:cellAddr colAddr="0" rowAddr="0"/><p:cellSz width="5000" height="1000"/></p:tc></p:tr>
              </p:tbl>
            </p:run></p:p></p:subList>
            <p:cellAddr colAddr="1" rowAddr="0"/><p:cellSpan colSpan="1" rowSpan="1"/>
            <p:cellSz width="10000" height="2000"/>
          </p:tc>
        </p:tr>
      </p:tbl>
      <p:t>표 뒤 글자</p:t>
    </p:run>
    <p:linesegarray><p:lineseg textpos="0" vertpos="0" vertsize="2000" textheight="2000" spacing="600" horzpos="0" horzsize="42000"/></p:linesegarray>
  </p:p>
</hs:sec>
"""

SECTION1 = """<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<hs:sec xmlns:hs="http://www.hancom.co.kr/hwpml/2011/section" xmlns:hp="http://www.hancom.co.kr/hwpml/2011/paragraph">
<hp:p id="0" paraPrIDRef="0"><hp:run charPrIDRef="0"><hp:t>둘째 구역 &amp; &lt;끝&gt;</hp:t></hp:run></hp:p>
</hs:sec>
"""

HPF = """<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<opf:package%s version="" unique-identifier="" id="">
  <opf:metadata><opf:title>XML 기능 &amp; 테스트</opf:title><opf:language>ko</opf:language></opf:metadata>
  <opf:manifest>
    <opf:item id="header" href="Contents/header.xml" media-type="application/xml"/>
    <opf:item id="body0" href="Contents/section0.xml" media-type="application/xml"/>
    <opf:item id="body1" href="Contents/section1.xml" media-type="application/xml"/>
  </opf:manifest>
  <opf:spine>
    <opf:itemref idref="header" linear="yes"/>
    <opf:itemref idref="body0" linear="yes"/>
    <opf:itemref idref="body1" linear="yes"/>
  </opf:spine>
</opf:package>
""" % NS

CONTAINER = """<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<ocf:container xmlns:ocf="urn:oasis:names:tc:opendocument:xmlns:container"><ocf:rootfiles>
<ocf:rootfile full-path="Contents/content.hpf" media-type="application/hwpml-package+xml"/>
</ocf:rootfiles></ocf:container>
"""


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xml_features.hwpx")
    fixed = (1980, 1, 1, 0, 0, 0)
    with zipfile.ZipFile(out, "w") as z:
        def add(name, data, stored=False):
            info = zipfile.ZipInfo(name, fixed)
            info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
            z.writestr(info, data.encode("utf-8"))
        add("mimetype", "application/hwp+zip", stored=True)
        add("Contents/header.xml", HEADER)
        add("Contents/section0.xml", SECTION0)
        add("Contents/section1.xml", SECTION1)
        add("Contents/content.hpf", HPF)
        add("META-INF/container.xml", CONTAINER)
    print("wrote", out)


if __name__ == "__main__":
    main()
