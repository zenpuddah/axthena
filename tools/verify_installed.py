#!/usr/bin/env python3
"""Exercise bundled resources in an isolated home, with no source/model calls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--home', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    home, binary = args.home.resolve(), args.binary.resolve()
    home.mkdir(parents=True, exist_ok=False)
    (home/'artifacts').mkdir()
    (home/'artifacts/models').symlink_to(root/'artifacts/models', target_is_directory=True)
    env = os.environ.copy()
    env.update(AXTHENA_HOME=str(home), AXTHENA_NEO4J_CONFIG=str(root/'artifacts/neo4j/connection.json'))
    env.pop('AXTHENA_RESOURCES', None)
    checks = []

    def command(*words):
        result = subprocess.run([str(binary), *map(str, words)], cwd=home, env=env, capture_output=True)
        if result.returncode:
            raise RuntimeError(result.stderr.decode())
        checks.append(' '.join(map(str, words)))
        return result.stdout

    assert b'neo4j-v1/store' in command('--help')
    status = json.loads(command('init'))
    assert status['backend'] == 'neo4j' and status['state'] == 'NOT_PREPARED'
    assert json.loads(command('status')) == status
    store = Path(status['store'])

    def retain(content):
        digest = hashlib.sha256(content).hexdigest()
        directory = store/'evidence'
        directory.mkdir(exist_ok=True)
        (directory/(digest+'.bin')).write_bytes(content)
        return {'sha256':digest, 'bytes':len(content)}

    body = b'Synthetic installation fixture, not a real source. No model answer is supplied.'
    ref, metadata = retain(body), retain(b'Installation verification fixture')
    report = {'number':1, 'report_id':'installed-fixture', 'title':'Synthetic installed fixture',
        'url':'https://fixture.invalid/installed', 'source_host':'fixture.invalid', 'body_text':body.decode(),
        'body_sha256':ref['sha256'], 'body':ref, 'capture_metadata':metadata, 'published_date':'2025-01-01',
        'source_available_at':None, 'collected_at':'2026-10-17', 'collection_run':'installed-fixture', 'author':None}
    imported = {'schema_version':'axthena.import.v1', 'reports':[report], 'artifacts':[ref,metadata],
        'source_runs':[{'run_id':'installed-fixture', 'status':'COMPLETE', 'started_at':'2026-10-17',
            'pages':[], 'plan':{'scope':'synthetic installation fixture only'}}],
        'source_change':'SYNTHETIC INSTALLATION FIXTURE'}
    source = home/'input.json'
    source.write_text(json.dumps(imported))
    command('import', '--input', source)
    assert json.loads(command('status'))['retained_reports'] == 1
    embedding = json.loads(command('embed', '--text', 'NVIDIA', '--role', 'query'))
    assert len(embedding['embedding']) == 768
    output = command('prepare', '--replay')
    meta = json.loads(output.decode().split('\nNodes:')[0])
    assert meta['reports_failed'] == 1 and meta['input_reports'] == 1
    assert meta['operations'][0]['reason'] == 'Replay missing retained model completion'
    request = home/'request.json'
    request.write_text(json.dumps({'query':'Q7', 'mode':'declared_date'}))
    answer = command('query', '--request', request)
    assert json.loads(answer)['distinct_reports'] == 1
    command('verify')
    export = home/'artifacts/export'
    command('export', '--out', export)
    backup, restored = home/'artifacts/backup', home/'artifacts/restored'
    command('backup', '--out', backup)
    command('restore', '--source', backup, '--store', restored)
    command('verify', '--store', restored)
    assert command('query', '--request', request, '--store', restored) == answer
    command('export', '--out', home/'artifacts/restored-export', '--store', restored)
    for name in ('graph.json','index.html','manifest.json','load.cypher','queries.cypher'):
        assert (export/name).read_bytes() == (home/'artifacts/restored-export'/name).read_bytes()
    results = {'commands':len(checks), 'command_lines':checks, 'binary':str(binary),
        'home':str(home), 'external_source_calls':0, 'model_calls':0,
        'encoder_dimensions':768, 'restored_queries_equal':True, 'exports_equal':5,
        'scope':'Installed resources with isolated home; explicit replay miss; real encoder/Neo4j; no model inference'}
    (home/'verification.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(results,indent=2))


if __name__ == '__main__':
    main()
