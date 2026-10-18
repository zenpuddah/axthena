#!/usr/bin/env python3
"""Exercise native tokenization/embeddings against the pinned package runner."""
import hashlib
import json
import math
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
output = root / 'artifacts/native-v1/encoder-check'
output.mkdir(parents=True, exist_ok=True)
policy = json.loads((root / 'config/v1.json').read_bytes())['encoder']
cases = json.loads((root / 'tests/fixtures/encoder-cases.json').read_bytes())

def run(text, role):
    command = [str(root / 'build/dev/axthena'), 'embed', '--text', text, '--role', role]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    value = json.loads(result.stdout)
    assert len(value['embedding']) == policy['dimensions']
    assert abs(sum(x*x for x in value['embedding']) - 1) < 1e-5
    assert value['tokens'][0] == 101 and value['tokens'][-1] == 102
    return value

text = cases['documents'][0]['text']
native = run(text, 'query')
command = [str(root / 'build/dev/bin/llama-embedding'), '-m', str(root / policy['file']),
           '-p', policy['query_prefix'] + text, '-c', '2048', '-b', '2048', '-ub', '2048',
           '-ngl', '0', '-t', '4', '--pooling', 'mean', '--attention', 'non-causal',
           '--embd-normalize', '2', '--embd-output-format', 'json', '--offline', '--no-warmup']
result = subprocess.run(command, check=True, capture_output=True, text=True)
(output / 'reference.stdout.json').write_text(result.stdout)
(output / 'reference.stderr.txt').write_text(result.stderr)
reference = json.loads(result.stdout)['data'][0]['embedding']
error = max(abs(a-b) for a, b in zip(native['embedding'], reference, strict=True))
assert error < 2e-6, error
documents = {doc['id']: run(doc['text'], 'document')['embedding'] for doc in cases['documents']}
rankings = []
for case in cases['queries']:
    vector = run(case['text'], 'query')['embedding']
    scores = sorted(((sum(a*b for a,b in zip(vector, v, strict=True)), key) for key,v in documents.items()), reverse=True)
    assert scores[0][1] == case['expected'], (case, scores)
    rankings.append({'query':case, 'ranking':[{'id':key,'cosine':score} for score,key in scores]})
record = {'schema_version':'axthena.encoder-result.v1', 'package':policy,
          'reference_command':command, 'maximum_absolute_difference':error,
          'native_tokens':native['tokens'], 'dimensions':len(reference),
          'alias_top1_correct':len(rankings), 'alias_cases':len(cases['queries']),
          'rankings':rankings, 'fixture_sha256':hashlib.sha256((root/'tests/fixtures/encoder-cases.json').read_bytes()).hexdigest()}
(output / 'result.json').write_text(json.dumps(record, indent=2)+'\n')
print(json.dumps(record, indent=2))
