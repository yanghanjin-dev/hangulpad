"""Generate the HangulPad regression document (external Hancom QA is separate)."""
from pathlib import Path
from xml.sax.saxutils import escape
import zipfile


def paragraph(text):
    return '<hp:p paraPrIDRef="0"><hp:run charPrIDRef="0"><hp:t>' + escape(text) + '</hp:t></hp:run></hp:p>'


def cell(row, col, text, height, rowspan=1, colspan=1, header=False, nested=""):
    width = 23764 * colspan
    return (f'<hp:tc header="{int(header)}" borderFillIDRef="0">'
            f'<hp:cellAddr rowAddr="{row}" colAddr="{col}"/>'
            f'<hp:cellSpan rowSpan="{rowspan}" colSpan="{colspan}"/>'
            f'<hp:cellSz width="{width}" height="{height}"/>'
            '<hp:cellMargin left="150" right="150" top="150" bottom="150"/>'
            '<hp:subList>' + paragraph(text) + nested + '</hp:subList></hp:tc>')


def main():
    root = Path(__file__).resolve().parent
    nested = ('<hp:tbl rowCnt="2" colCnt="1" repeatHeader="0"><hp:sz width="22500" height="6000"/>'
              '<hp:tr>' + cell(0, 0, 'NESTED CELL A', 3000) + '</hp:tr>'
              '<hp:tr>' + cell(1, 0, 'NESTED CELL B', 3000) + '</hp:tr></hp:tbl>')
    rows = ['<hp:tr>' + cell(0, 0, 'REPEATED HEADER - MERGED CELL CONTINUATION', 4000,
                            colspan=2, header=True) + '</hp:tr>']
    rows.append('<hp:tr>' + cell(1, 0, '\n'.join(f'MERGED LEFT - LINE {i:02d}' for i in range(1, 66)),
                                             90000, rowspan=3)
                + cell(1, 1, '\n'.join(f'RIGHT FIRST - LINE {i:02d}' for i in range(1, 15)), 30000) + '</hp:tr>')
    rows.append('<hp:tr>' + cell(2, 1, '\n'.join(f'RIGHT MIDDLE - LINE {i:02d}' for i in range(1, 15)),
                               30000) + '</hp:tr>')
    rows.append('<hp:tr>' + cell(3, 1, '\n'.join(f'RIGHT LAST - LINE {i:02d}' for i in range(1, 15)),
                               30000, nested=nested) + '</hp:tr>')
    xml = ('<?xml version="1.0" encoding="UTF-8"?>'
           '<hs:sec xmlns:hs="http://www.hancom.co.kr/hwpml/2011/section" '
           'xmlns:hp="http://www.hancom.co.kr/hwpml/2011/paragraph">'
           '<hp:p><hp:run><hp:secPr><hp:pagePr width="59528" height="84188">'
           '<hp:margin left="6000" right="6000" top="5600" bottom="4252"/>'
           '</hp:pagePr></hp:secPr><hp:tbl rowCnt="4" colCnt="2" repeatHeader="1" pageBreak="CELL">'
           '<hp:sz width="47528" height="94000"/>' + ''.join(rows) + '</hp:tbl></hp:run></hp:p></hs:sec>')
    target = root / 'table_pagination.hwpx'
    with zipfile.ZipFile(root / 'simple_paragraph.hwpx') as source, zipfile.ZipFile(target, 'w') as output:
        for entry in source.infolist():
            data = xml.encode('utf-8') if entry.filename == 'Contents/section0.xml' else source.read(entry.filename)
            output.writestr(entry, data)
    print(target)


if __name__ == '__main__':
    main()
