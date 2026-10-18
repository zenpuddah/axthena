#!/usr/bin/env python3
"""Deterministic stand-in for the external executable; never makes a network call."""
import json
import re
import sys
import time

if sys.argv[1:] == ['--version']:
    print('opencode v2.0.30')
    sys.exit(0)

request = json.loads(sys.argv[-1])
prompt = request['prompt']
task = re.search(r'Echo task_id exactly: (task:[0-9a-f]+)', prompt)[1]
if prompt.startswith('TIMEOUT'):
    print('incomplete response', flush=True)
    time.sleep(60)
elif prompt.startswith('REFUSE'):
    print(json.dumps({'data': {'text': 'I cannot provide the requested output.'}}))
elif prompt.startswith('TRUNCATE'):
    print('{"data":{"text":')
elif prompt.startswith('FAIL'):
    print(json.dumps({'error': 'synthetic provider failure'}))
    sys.exit(7)
else:
    answer = {'task_id': 'wrong' if prompt.startswith('WRONG') else task, 'value': 'retained'}
    print(json.dumps({'data': {'text': json.dumps(answer)}}))
