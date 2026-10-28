"""Seeds for fuzz_hwp_records: u32 DocInfo length + DocInfo + BodyText/Section0,
both inflated. Usage: python3 make_hwp_record_seeds.py OUT_DIR file.hwp ...
(needs the olefile package)."""
import os, struct, sys, zlib
import olefile

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
for i, path in enumerate(sys.argv[2:]):
    try:
        f = olefile.OleFileIO(path)
        flags = struct.unpack('<I', f.openstream('FileHeader').read()[36:40])[0]
        if flags & 6:
            continue
        def read(name):
            d = f.openstream(name).read()
            return zlib.decompress(d, -15) if flags & 1 else d
        di, sec = read('DocInfo'), read('BodyText/Section0')
        with open(os.path.join(out, 'seed%03d' % i), 'wb') as o:
            o.write(struct.pack('<I', len(di)) + di + sec)
    except Exception as e:  # damaged samples are skipped
        print('skip', path, e)
