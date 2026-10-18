#!/usr/bin/env python3
"""Verify the native graph, materialize real-ID query examples and measure reads."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--graph', type=Path, required=True)
parser.add_argument('--store', type=Path, default=root/'artifacts/neo4j-v1/store')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--binary', type=Path, default=root/'build/dev/axthena')
parser.add_argument('--expected-reports', type=int)
parser.add_argument('--from', dest='from_date')
parser.add_argument('--to', dest='to_date')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
graph = json.loads(args.graph.read_bytes())
nodes = {n['id']: n for n in graph['nodes']}
reports = sorted((n for n in nodes.values() if n['kind']=='Report'), key=lambda n:n['props']['number'])
assert reports and len({r['props']['report_id'] for r in reports})==len(reports)
if args.expected_reports is not None:assert len(reports)==args.expected_reports
checks = Counter()
for n in nodes.values():
    for pid in n['support']:
        assert nodes[pid]['kind']=='Passage'
        checks['support_reference'] += 1
    if n['kind']=='Passage':
        p=n['props']; body=nodes[p['report']]['props']['body_text'].encode()
        assert p['unit']=='UTF8_BYTES'
        assert body[p['begin']:p['end']].decode()==p['quote']
        assert hashlib.sha256(body).hexdigest()==p['body_sha256']
        checks['exact_passage'] += 1
    if n['kind']=='ResolutionAttempt':
        packet=n['props']['candidates']
        name={x['id'] for x in packet['name_candidates']}; vector={x['id'] for x in packet['vector_candidates']}
        assert {x['id'] for x in packet['union']}==name|vector
        for c in packet['union']:
            assert c['intersection']==(c['id'] in name&vector)
        checks['actual_retrieval_union'] += 1
    if n['kind']=='Expression':
        actual=set()
        for edge in graph['edges']:
            if edge['from']==n['id'] and edge['kind']=='CONCERNS_ACTIVITY':
                a=nodes[edge['to']]['props'];actual.add(a['organisation'])
        expected={s['organisation'] for s in n['props']['complete_meaning']['subjects'] if s['domain'] is not None}
        assert actual==expected
        checks['paired_activity_subject'] += 1
for e in graph['edges']:
    assert e['from'] in nodes and e['to'] in nodes
    assert all(p in nodes and nodes[p]['kind']=='Passage' for p in e['support'])
    checks['edge_reference'] += 1
for r in reports:
    body=r['props']['body_text'].encode(); assert hashlib.sha256(body).hexdigest()==r['props']['body_sha256']

def decode_cypher_literal(text):
    converted=[];quoted=False;escaped=False;at=0
    while at<len(text):
        c=text[at]
        if quoted:
            converted.append(c)
            if escaped:escaped=False
            elif c=='\\':escaped=True
            elif c=='"':quoted=False
        elif c=='"':quoted=True;converted.append(c)
        elif c=='`':
            end=text.index('`',at+1);converted.append(json.dumps(text[at+1:end]));at=end
        else:converted.append(c)
        at+=1
    return json.loads(''.join(converted))

loader=(args.graph.parent/'load.cypher').read_text()
node_literal,edge_rest=loader.split('WITH ',1)[1].split(' AS nodes,\n',1)
edge_literal=edge_rest.split(' AS edges\n',1)[0]
node_rows=decode_cypher_literal(node_literal);edge_rows=decode_cypher_literal(edge_literal)
assert len(node_rows)==len(nodes) and len(edge_rows)==len(graph['edges'])
projection={row['props']['native_id']:row for row in node_rows}
for native_id,row in projection.items():
    n=nodes[native_id];props=row['props']
    assert row['kind']==props['kind']==n['kind']
    assert props['id']==graph['meta']['run_id']+'/'+native_id
    assert props['name']==n['label'] and json.loads(props['support_json'])==n['support']
    for key,value in n['props'].items():
        if key in ('id','native_id','name','run_id','kind','support_json'):key='data_'+key
        assert (json.loads(props[key+'_json']) if isinstance(value,(dict,list)) else props[key])==value
    checks['cypher_node_roundtrip']+=1
for row,e in zip(edge_rows,graph['edges'],strict=True):
    assert row['kind']==e['kind'] and row['props']['native_id']==e['id']
    assert row['from']==graph['meta']['run_id']+'/'+e['from'] and row['to']==graph['meta']['run_id']+'/'+e['to']
    assert json.loads(row['props']['properties_json'])==e['props']
    checks['cypher_edge_roundtrip']+=1
manifest=json.loads((args.graph.parent/'manifest.json').read_bytes())
for filename,key in [('graph.json','graph_sha256'),('index.html','html_sha256'),('load.cypher','cypher_sha256')]:
    assert hashlib.sha256((args.graph.parent/filename).read_bytes()).hexdigest()==manifest[key]
    checks['export_digest']+=1

expressions=[n for n in nodes.values() if n['kind']=='Expression']
idea_counts=Counter(e['to'] for e in graph['edges'] if e['kind']=='EXPRESSES')
idea=max(idea_counts,key=idea_counts.get)
claims=[n for n in nodes.values() if n['kind']=='Claim']
claim=next((n['id'] for n in claims if n['props']['status']=='CONFIRMED_SCOPED'),claims[0]['id'] if claims else None)
trace=next((n['id'] for n in expressions if nodes[n['props']['report']]['props']['number']==17),reports[0]['id'])
dates=sorted(r['props']['published_date'] for r in reports if r['props'].get('published_date'))
common={'mode':'declared_date','from':args.from_date or dates[0],'to':args.to_date or dates[-1],'limit':2000}
specs={
 '01-stock-evidence':{'query':'Q1','stocks':['NVDA.US']},
 '02-company-neighbourhood':{'query':'Q2','stocks':['NVDA.US']},
 '03-shared-idea':{'query':'Q3','id':idea},
 '04-pair-evidence':{'query':'Q4','stocks':['NVDA.US','MSFT.US']},
  '05-claim-history':{'query':'Q5',**({'id':claim} if claim else {})},
 '06-evidence-trace':{'query':'Q6','id':trace},
 '07-coverage':{'query':'Q7'},
 '08-universe-opinions':{'query':'Q1','kind':'OPINION','stocks':[]},
 '09-strict-history':{'query':'Q1','mode':'strict','stocks':['NVDA.US','MSFT.US']},
}
summaries=[]
for name,spec in specs.items():
    spec=common|spec; request=args.output/(name+'.json');request.write_text(json.dumps(spec,indent=2)+'\n')
    begin=time.perf_counter();result=subprocess.run([str(args.binary),'query','--store',str(args.store),'--run',graph['meta']['run_id'],'--request',str(request)],capture_output=True,check=True)
    elapsed=(time.perf_counter()-begin)*1000
    data=json.loads(result.stdout);(args.output/(name+'.result.json')).write_bytes(result.stdout)
    if spec['mode']=='strict' and all(r['props']['source_available_at'] is None for r in reports):assert data['distinct_reports']==0 and len(data['coverage']['exclusions'])==len(reports)
    summaries.append({'query':name,'elapsed_ms':elapsed,'distinct_reports':data['distinct_reports'],'nodes':len(data['nodes']),'truncated':data['truncated'],'result_sha256':hashlib.sha256(result.stdout).hexdigest()})
query=args.output/'01-stock-evidence.json';warm=[]
for _ in range(5):
    begin=time.perf_counter();result=subprocess.run([str(args.binary),'query','--store',str(args.store),'--run',graph['meta']['run_id'],'--request',str(query)],capture_output=True,check=True)
    warm.append((time.perf_counter()-begin)*1000)
    assert hashlib.sha256(result.stdout).hexdigest()==summaries[0]['result_sha256']
backend=json.loads(subprocess.run([str(args.binary),'status','--store',str(args.store)],capture_output=True,check=True).stdout)
record={'schema_version':'axthena.review-verification.v1','run_id':graph['meta']['run_id'],
        'nodes':dict(Counter(n['kind'] for n in nodes.values())),
        'edges':dict(Counter(e['kind'] for e in graph['edges'])),
        'report_status':dict(Counter(r['props']['processing_status'] for r in reports)),
        'mechanical_checks':dict(checks),'queries':summaries,
        'warm_stock_query_ms':{'samples':warm,'median':statistics.median(warm),'max':max(warm)},
        'measurement_scope':'whole CLI startup, retained-evidence verification and JSON output; warm filesystem after preparation; no cold-cache or scale claim',
        'platform':platform.platform(),'backend':backend['backend'],'namespace':backend['namespace'],
        'archive_bytes':sum(p.stat().st_size for p in args.store.rglob('*') if p.is_file()),
        'notes':['No live source/model calls in this verification.','Real-corpus shared-meaning multiplicities: '+str(sorted(idea_counts.values(),reverse=True)),
                  'Application reads execute in Neo4j; portable load.cypher payloads are round-trip checked separately.',
                 'All exact-version historical availability unknown; declared-date mode is exploratory.']}
(args.output/'verification.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
