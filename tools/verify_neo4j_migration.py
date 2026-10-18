#!/usr/bin/env python3
"""Compare preserved SQLite runs and frozen CLI reads with migrated Neo4j data."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT/'build/dev/axthena')
    parser.add_argument('--store', type=Path, default=ROOT/'artifacts/neo4j-v1/pilot-store')
    parser.add_argument('--legacy', type=Path, default=ROOT/'artifacts/native-v1/store')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    marker = json.loads((args.store/'neo4j-store.json').read_text())
    connection = json.loads((ROOT/'artifacts/neo4j/connection.json').read_text())
    auth = base64.b64encode((connection['user']+':'+Path(connection['password_file']).read_text()).encode()).decode()

    def cypher(statement, **parameters):
        parameters['store'] = marker['namespace']
        request = urllib.request.Request(connection['endpoint']+'/db/'+connection['database']+'/query/v2',
            data=json.dumps({'statement':statement, 'parameters':parameters, 'accessMode':'Read'}).encode(),
            headers={'Authorization':'Basic '+auth, 'Content-Type':'application/json'})
        with urllib.request.urlopen(request, timeout=120) as response:
            result = json.load(response)
        assert not result.get('errors'), result.get('errors')
        return [dict(zip(result['data']['fields'], row, strict=True)) for row in result['data']['values']]

    baseline = json.loads((ROOT/'artifacts/neo4j-v1/legacy-preservation.json').read_text())
    for name, digest in baseline.items():
        with (ROOT/name).open('rb') as source:
            assert hashlib.file_digest(source, 'sha256').hexdigest() == digest, name
    checks = {'preserved_legacy_files':len(baseline), 'equal_runs':0, 'equal_nodes':0, 'equal_edges':0}
    with sqlite3.connect((args.legacy/'store.sqlite').resolve().as_uri()+'?mode=ro', uri=True) as legacy:
        legacy.execute('PRAGMA query_only=ON')
        runs = list(legacy.execute('SELECT id,graph FROM runs ORDER BY rowid'))
        actual = cypher('MATCH (r:AxRun {store:$store}) RETURN r.id AS id ORDER BY r.sequence')
        assert [r['id'] for r in actual] == [r[0] for r in runs]
        for run, raw in runs:
            graph = json.loads(raw)
            meta = cypher('MATCH (r:AxRun {store:$store,id:$run}) RETURN r.payload AS payload', run=run)
            assert json.loads(meta[0]['payload']) == graph['meta']
            for family, statement in (
                ('nodes','MATCH (n:AxRecord {store:$store,run:$run}) RETURN n.payload AS payload,n.digest AS digest ORDER BY n.id'),
                ('edges','MATCH (:AxRecord {store:$store,run:$run})-[e]->() RETURN e.payload AS payload,e.digest AS digest ORDER BY e.id'),
            ):
                rows = cypher(statement, run=run)
                assert [json.loads(r['payload']) for r in rows] == graph[family]
                assert all(hashlib.sha256(r['payload'].encode()).hexdigest()==r['digest'] for r in rows)
                checks['equal_'+family] += len(rows)
            checks['equal_runs'] += 1
    samples = []
    for index, row in enumerate(json.loads((ROOT/'artifacts/neo4j-v1/legacy-queries.json').read_text())):
        request = args.out.parent/f'migration-query-{index}.json'
        request.write_text(json.dumps(row['request'])+'\n')
        begin = time.perf_counter()
        result = subprocess.run([str(args.binary),'query','--store',str(args.store),'--request',str(request)],capture_output=True,check=True)
        assert json.loads(result.stdout) == row['result'], row['request']
        samples.append({'request':row['request'], 'elapsed_ms':(time.perf_counter()-begin)*1000})
    checks['equal_original_binary_queries'] = len(samples)
    output = {'checks':checks, 'query_measurements':samples,
        'scope':'Warm local CLI/HTTP reads on retained pilot; no scale or speed superiority claim',
        'legacy':str(args.legacy), 'neo4j_namespace':marker['namespace']}
    with args.out.open('x') as out:
        json.dump(output,out,indent=2)
        out.write('\n')
    print(json.dumps(checks,indent=2))


if __name__ == '__main__':
    main()
