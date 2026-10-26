"""Compare selected native GDI page images; update only after visual review.
Requires Pillow. Source PDFs are independent references, not these baselines.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    'ethics': ('sample/corpus/ethics.hwpx', 17, [1, 2, 3, 5, 6, 13, 14, 17]),
    'gyuje': ('sample/corpus/gyuje-규제합리화.hwpx', 20, [1, 4, 18, 20]),
    'mfds': ('sample/corpus/mfds-프로프라놀롤.hwpx', 4, [1, 3, 4]),
    'merged': ('tests/fixtures/table_pagination.hwpx', 2, [1, 2]),
}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--renderer', type=Path, default=ROOT/'build/windows-release/layout_render.exe')
    ap.add_argument('--output', type=Path, default=ROOT/'build/visual-regression')
    ap.add_argument('--baseline', type=Path, default=ROOT/'tests/visual/windows')
    ap.add_argument('--update', action='store_true', help='explicitly replace reviewed baselines')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    args.baseline.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env['PATH'] = r'C:\msys64\ucrt64\bin;' + env.get('PATH', '')
    report = {'platform': platform.platform(), 'dpi': 96, 'cases': {}}
    failures = []
    baseline_manifest = args.baseline/'manifest.json'
    old = json.loads(baseline_manifest.read_text(encoding='utf-8')) if baseline_manifest.exists() else {}
    for key, (source, expected, selected) in CASES.items():
        out = args.output/key
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(args.renderer.resolve()), str(ROOT/source), str(out.resolve())], env=env, check=True)
        layout = json.loads((out/'layout.json').read_text(encoding='utf-8'))
        if layout['pages'] != expected or layout['fallback_cuts'] or layout['suppressed_headers']:
            failures.append(f'{key}: layout count/cut regression')
        ends = {}
        for piece in layout['pieces']:
            block = piece['block']
            if piece['offset'] != ends.get(block, 0):
                failures.append(f'{key}: non-contiguous block {block}')
            ends[block] = piece['offset'] + piece['height']
        case = {'source_sha256': hashlib.sha256((ROOT/source).read_bytes()).hexdigest(),
                'pages': layout['pages'], 'fallback_cuts': layout['fallback_cuts'], 'images': {}}
        for page in selected:
            name = f'{key}-{page:03d}.png'
            image = Image.open(out/f'page-{page:03d}.bmp').convert('RGB')
            image.save(args.output/name)
            target = args.baseline/name
            case['images'][name] = hashlib.sha256(image.tobytes()).hexdigest()
            if args.update:
                image.save(target)
            elif not target.exists():
                failures.append(f'{name}: missing baseline (review before --update)')
            else:
                previous = Image.open(target).convert('RGB')
                if previous.size != image.size:
                    failures.append(f'{name}: page size changed')
                else:
                    diff = ImageChops.difference(previous, image)
                    if diff.getbbox():
                        diff.save(args.output/f'diff-{name}')
                        failures.append(f'{name}: pixels changed')
        if not args.update and old.get('cases', {}).get(key, {}).get('source_sha256') != case['source_sha256']:
            failures.append(f'{key}: source changed')
        report['cases'][key] = case
    (args.output/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    if args.update and not failures:
        baseline_manifest.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    for failure in failures:
        print(failure)
    print(f'{len(failures)} failures; native visual comparison (font/Windows changes can alter pixels)')
    return bool(failures)

if __name__ == '__main__':
    raise SystemExit(main())

