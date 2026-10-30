/* HangulPad — namespace declarations and XML declaration that 한글 writes on
 * every HWPX XML part (shared by the HWPX writer and the HWP converter). */
#ifndef HP_HWPX_NS_H
#define HP_HWPX_NS_H

#define HP_NS_ALL                                                                                  \
    " xmlns:ha=\"http://www.hancom.co.kr/hwpml/2011/app\""                                         \
    " xmlns:hp=\"http://www.hancom.co.kr/hwpml/2011/paragraph\""                                   \
    " xmlns:hp10=\"http://www.hancom.co.kr/hwpml/2016/paragraph\""                                 \
    " xmlns:hs=\"http://www.hancom.co.kr/hwpml/2011/section\""                                     \
    " xmlns:hc=\"http://www.hancom.co.kr/hwpml/2011/core\""                                        \
    " xmlns:hh=\"http://www.hancom.co.kr/hwpml/2011/head\""                                        \
    " xmlns:hhs=\"http://www.hancom.co.kr/hwpml/2011/history\""                                    \
    " xmlns:hm=\"http://www.hancom.co.kr/hwpml/2011/master-page\""                                 \
    " xmlns:hpf=\"http://www.hancom.co.kr/schema/2011/hpf\""                                       \
    " xmlns:dc=\"http://purl.org/dc/elements/1.1/\""                                               \
    " xmlns:opf=\"http://www.idpf.org/2007/opf/\""                                                 \
    " xmlns:ooxmlchart=\"http://www.hancom.co.kr/hwpml/2016/ooxmlchart\""                          \
    " xmlns:hwpunitchar=\"http://www.hancom.co.kr/hwpml/2016/HwpUnitChar\""                        \
    " xmlns:epub=\"http://www.idpf.org/2007/ops\""                                                 \
    " xmlns:config=\"urn:oasis:names:tc:opendocument:xmlns:config:1.0\""

#define HP_XML_DECL "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"

#endif /* HP_HWPX_NS_H */
