#!/usr/bin/env python3
"""Export pinned, real database query results for an explicitly frozen UI demo."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('--url', default='http://127.0.0.1:17780')
parser.add_argument('--out', default='workspace/public/demo.json')
args = parser.parse_args()
with urllib.request.urlopen(args.url+'/api/status') as response:
    run = json.load(response)['latest_run']
plan = {'intent': 'overview', 'stocks': ['NVDA.US'], 'from': '2025-01-01', 'to': '2025-12-31', 'mode': 'declared_date', 'max_hops': 0, 'limit': 5000}
data = {}
for name, patch in [('nvidia', {}), ('microsoft', {'stocks': ['MSFT.US']}), ('compare', {'intent': 'compare', 'stocks': ['NVDA.US', 'MSFT.US']}), ('strict', {'mode': 'strict'})]:
    request = urllib.request.Request(args.url+'/api/investigate', data=json.dumps({'run': run, 'plan': {**plan, **patch}}).encode(), headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=60) as response:
        answer = json.load(response)
    assert answer['run_id'] == run
    # UI needs exact quoted passages and source metadata, not complete article
    # bodies or acquisition HTTP headers. The original store remains unchanged.
    for result in answer['results']:
        for node in result['nodes']:
            if node['kind'] == 'Report':
                node['props'].pop('body_text', None)
        result['coverage']['collection'] = []
    data[name] = answer
raw = (json.dumps(data, ensure_ascii=False, separators=(',', ':'))+'\n').encode()
out = Path(args.out)
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(raw)
manifest = {'run_id': run, 'mode': 'frozen query snapshots; no live AI or database', 'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw), 'views': list(data)}
out.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(json.dumps(manifest, indent=2))
