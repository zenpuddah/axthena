#!/usr/bin/env python3
"""Offline read/reapplication/backup equivalence for an already prepared run."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--store', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--review', type=Path, required=True)
    parser.add_argument('--restore', type=Path, required=True)
    parser.add_argument('--export', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()

    def inventory(root):
        return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(root.rglob('*')) if p.is_file()}

    def command(*words, store=None):
        result = subprocess.run([str(args.binary), *map(str, words), '--store', str(store or args.store)],
                                capture_output=True)
        if result.returncode:
            raise RuntimeError(result.stderr.decode())
        return result

    before = inventory(args.store)
    graph = json.loads((args.review/'graph.json').read_bytes())
    command('verify')
    replay = command('prepare', '--replay', '--config', args.config)
    meta = json.loads(replay.stdout.decode().split('\nNodes:')[0])
    assert meta['run_id'] == graph['meta']['run_id']
    command('backup', '--out', args.restore)
    command('verify', store=args.restore)
    query_count = 0
    for request in sorted((args.review/'reads').glob('*.json')):
        if request.name.endswith('.result.json') or request.name == 'verification.json':
            continue
        original = command('query', '--request', request).stdout
        recovered = command('query', '--request', request, store=args.restore).stdout
        assert original == recovered == request.with_name(request.stem+'.result.json').read_bytes()
        query_count += 1
    assert query_count >= 7
    command('export', '--out', args.export, store=args.restore)
    exports = ['graph.json', 'index.html', 'manifest.json', 'load.cypher', 'queries.cypher']
    for name in exports:
        assert (args.review/name).read_bytes() == (args.export/name).read_bytes(), name
    after = inventory(args.store)
    assert before == after, 'Reads/reapplication changed store files'
    restored = inventory(args.restore)
    bundle = json.loads((args.restore/'database-backup.json').read_bytes())
    for name, reference in bundle['archive'].items():
        assert restored[name] == before[name] == reference['sha256'], name
    source_status = json.loads(command('status').stdout)
    restored_status = json.loads(command('status', store=args.restore).stdout)
    assert source_status['backend'] == restored_status['backend'] == 'neo4j'
    assert source_status['namespace'] != restored_status['namespace']
    for key in ('published_runs', 'latest_run', 'corpus_id', 'retained_reports', 'collection_receipts'):
        assert source_status[key] == restored_status[key], key
    assert len(bundle['runs']) == source_status['published_runs']
    result = {'run_id': meta['run_id'], 'unchanged_store_files': len(before),
              'equal_archive_files': len(bundle['archive']),
              'restored_runs': len(bundle['runs']), 'restored_documents': len(bundle['documents']),
              'equal_queries': query_count, 'equal_exports': len(exports),
              'reapplication': 'same published run; all store files unchanged; replay forbids generation',
              'restore': str(args.restore), 'regenerated_export': str(args.export),
              'backend': 'Neo4j; restored into an independent namespace'}
    with args.out.open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()
