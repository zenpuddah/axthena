#!/usr/bin/env python3
"""Download one pinned public encoder artifact inside this component."""
import hashlib
import json
from pathlib import Path
from urllib.request import urlopen

root = Path(__file__).resolve().parents[1]
cfg = json.loads((root / 'config/v1.json').read_bytes())['encoder']
output = root / cfg['file']
output.parent.mkdir(parents=True, exist_ok=True)
url = ('https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/'
       '0188c9bf409793f810680a5a431e7b899c46104c/nomic-embed-text-v1.5.f16.gguf')
if not output.exists():
    partial = output.with_suffix('.download')
    with partial.open('xb') as target, urlopen(url, timeout=120) as source:
        while chunk := source.read(1024 * 1024):
            target.write(chunk)
    if hashlib.file_digest(partial.open('rb'), 'sha256').hexdigest() != cfg['sha256']:
        raise SystemExit('Downloaded model hash mismatch; partial retained')
    partial.rename(output)
with output.open('rb') as source:
    assert hashlib.file_digest(source, 'sha256').hexdigest() == cfg['sha256']
print('Verified encoder:', output, output.stat().st_size, 'bytes')
