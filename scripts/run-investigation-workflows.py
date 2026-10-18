#!/usr/bin/env python3
"""Retain reproducible read-only investigations against a running local workspace."""
import argparse
from collections import Counter
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import urllib.error
import urllib.request
from urllib.parse import urlsplit


def write(path, value):
    data = value if isinstance(value, bytes) else (json.dumps(value, indent=2, ensure_ascii=False)+'\n').encode()
    with path.open('xb') as stream:
        stream.write(data)
    return hashlib.sha256(data).hexdigest()


def require(value, message):
    if not value:
        raise ValueError(message)


def reports(result):
    return {node['id'] for node in result['nodes'] if node['kind'] == 'Report'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:17780')
    parser.add_argument('--recipes', type=Path, default=Path(__file__).resolve().parents[1]/'examples/investigation-workflows.json')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    url = urlsplit(args.url)
    require(url.scheme == 'http' and url.hostname in ('127.0.0.1', 'localhost', '::1')
            and not url.username and not url.password and url.path in ('', '/')
            and not url.query and not url.fragment, 'Expected local workspace HTTP origin')
    recipe_bytes = args.recipes.read_bytes()
    recipes = json.loads(recipe_bytes)
    require(recipes['schema_version'] == 'axthena.investigation-recipes.v1', 'Unsupported recipe schema')
    run = recipes['run']
    require(isinstance(run, str) and run.startswith('run:'), 'Recipes must pin a published run')
    names = [case['name'] for case in recipes['cases']]
    require(len(names) == len(set(names)) and all(re.fullmatch('[a-z][a-z0-9-]{0,63}', name) for name in names), 'Invalid/duplicate recipe names')
    require('claim-evidence' not in names, 'claim-evidence is reserved for the generated follow-up')
    args.out.mkdir(parents=True, exist_ok=False)
    write(args.out/'recipes.json', recipe_bytes)
    summary = {'schema_version':'axthena.investigation-review.v1', 'run':run,
               'created_at':dt.datetime.now(dt.timezone.utc).isoformat(), 'origin':args.url,
               'recipes_sha256':hashlib.sha256(recipe_bytes).hexdigest(), 'cases':{}}

    def query(name, plan):
        body = json.dumps({'run':run, 'plan':plan}).encode()
        request_hash = write(args.out/f'{name}.request.json', body)
        request = urllib.request.Request(args.url.rstrip('/')+'/api/investigate', data=body,
                                         headers={'Content-Type':'application/json'})
        try:
            response = urllib.request.urlopen(request, timeout=60)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            raw = response.read()
            http_status = response.status
        response_hash = write(args.out/f'{name}.response.json', raw)
        require(http_status == 200, f'{name}: HTTP {http_status}; response retained')
        answer = json.loads(raw)
        require(answer['run_id'] == run, f'{name}: selected run changed')
        expected_results = 2 if plan['intent'] == 'compare' else 1
        require(len(answer['results']) == expected_results, f'{name}: unexpected result count')
        parts = []
        for index, result in enumerate(answer['results']):
            require(result['run_id'] == run and not result['truncated'], f'{name}: wrong run or incomplete result')
            require(len({n['id'] for n in result['nodes']}) == len(result['nodes']), f'{name}: duplicate record IDs')
            require(len(reports(result)) == result['distinct_reports'], f'{name}: report count differs from identities')
            if plan['intent'] == 'compare':
                require(result['request']['stocks'] == [plan['stocks'][index]], f'{name}: company scopes mixed')
            parts.append({'stocks':result['request']['stocks'], 'reports':result['distinct_reports'],
                          'records_by_kind':dict(sorted(Counter(n['kind'] for n in result['nodes']).items())),
                          'claim_statuses':dict(sorted(Counter(n['props']['status'] for n in result['nodes'] if n['kind']=='Claim').items())),
                          'excluded_reports':len(result['coverage']['exclusions']),
                          'grouping_exclusions':len(result['grouping_exclusions']), 'truncated':False})
        summary['cases'][name] = {'plan':answer['plan'], 'parts':parts,
                                  'request_sha256':request_hash, 'response_sha256':response_hash}
        return answer

    try:
        answers = {case['name']:query(case['name'], {**recipes['base'], **case['plan']}) for case in recipes['cases']}
        comparison = answers.get('company-comparison')
        if comparison:
            a, b = [reports(result) for result in comparison['results']]
            summary['company_publications'] = {'union':len(a|b), 'shared':sorted(a&b),
                                               'first_only':len(a-b), 'second_only':len(b-a)}
        if 'nvidia-first-half' in answers and 'nvidia-second-half' in answers:
            a = reports(answers['nvidia-first-half']['results'][0])
            b = reports(answers['nvidia-second-half']['results'][0])
            require(not a&b, 'Disjoint publication periods share report identities')
            summary['period_publications'] = {'first_half':len(a), 'second_half':len(b), 'union':len(a|b)}
        claims = answers.get('reported-claims')
        if claims:
            selected = next((n for n in claims['results'][0]['nodes'] if n['kind']=='Claim' and n.get('evidence')), None)
            if selected:
                followup = query('claim-evidence', {**claims['plan'], 'intent':'evidence', 'id':selected['id']})
                returned = next(n for n in followup['results'][0]['nodes'] if n['id']==selected['id'])
                require(returned['evidence'] == selected['evidence'], 'Claim follow-up changed its exact evidence')
                summary['evidence_followup'] = {'claim':selected['id'], 'label':selected['label'],
                                               'status':selected['props']['status'],
                                               'passages':[p['id'] for p in returned['evidence']], 'exact_evidence_equal':True}
            else:
                summary['evidence_followup'] = {'state':'NO_SUPPORTED_CLAIM_RETURNED'}
        summary['state'] = 'VERIFIED'
        write(args.out/'summary.json', summary)
    except Exception as error:
        write(args.out/'failure.json', {'error':str(error), 'completed':summary})
        raise
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
