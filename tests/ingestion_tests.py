#!/usr/bin/env python3
"""Offline transport/recovery contrasts and optional real native/Neo4j integration."""
import base64
import copy
import datetime as dt
import io
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import tempfile
import unittest
from unittest.mock import patch
import urllib.request

COMPONENT = Path(__file__).resolve().parents[1]
sys.path.insert(0, os.environ.get('AXTHENA_INGEST_TOOLS', str(COMPONENT/'tools')))
import ingest
from ingestors import ADAPTERS

URL = 'https://publisher.invalid/wp-json/wp/v2/posts'


def article(number=1, body='First body'):
    return {'id': number, 'date': '2025-01-15T12:00:00', 'link': f'https://publisher.invalid/posts/{number}',
            'title': {'rendered': f'Article {number}'}, 'content': {'rendered': f'<p>{body}</p>'}}


class IngestionTests(unittest.TestCase):
    def setUp(self):
        (COMPONENT/'artifacts/test-work').mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(prefix='ingestion-', dir=COMPONENT/'artifacts/test-work')
        self.root = Path(self.tmp.name)
        ingest.STOP.clear()
        self.options = {**ingest.FETCH, 'interval_seconds': 0, 'retry_base_seconds': 0}

    def tearDown(self):
        ingest.STOP.clear()
        self.tmp.cleanup()

    def transport(self, job, fetch, maximum=10):
        return ingest.Transport(self.root/'transport', self.root/job, maximum, fetch, wait=lambda _: None)

    def config(self, **collection):
        raw = {'schema_version': 'axthena.ingestion.v1', 'store': str(self.root/'store'),
               'collection': {'from': '2025-01-01', 'to': '2025-01-31', 'articles_per_scope': 10,
                              'pages_per_scope': 3, 'max_requests': 10, **collection},
               'sources': [{'id': 'fixture', 'name': 'Fixture', 'ingestor': 'wordpress', 'endpoint': URL}],
               'fetch': {'interval_seconds': 0, 'retry_base_seconds': 0}, 'processing': {'prepare': False}}
        path = self.root/'config.json'
        ingest.save(path, raw)
        return ingest.configuration(path)

    def test_dry_run_and_unknown_config_fail_closed(self):
        config = self.config()
        with patch.object(ingest, 'run_job', side_effect=AssertionError('Dry run invoked work')), context_stdout():
            self.assertEqual(ingest.main(['--native', '/not-needed', '--config', str(self.root/'config.json'), '--dry-run']), 0)
        self.assertFalse(Path(config['store']).exists())
        raw = ingest.load(self.root/'config.json')
        raw['fetch']['concurency'] = 3
        ingest.save(self.root/'config.json', raw)
        with self.assertRaises(ValueError):
            ingest.configuration(self.root/'config.json')

    def test_resume_and_cross_job_fresh_cache_make_no_requests(self):
        fetch = unittest.mock.Mock(return_value=(200, {'etag': 'v1'}, b'[]'))
        first = self.transport('one', fetch).get(URL, self.options)
        second = self.transport('one', fetch).get(URL, self.options)
        third = self.transport('two', fetch).get(URL, self.options)
        self.assertEqual(fetch.call_count, 1)
        self.assertEqual(first, second)
        self.assertEqual(third['cache'], 'fresh')
        self.assertEqual(first['body_collected_at'], third['body_collected_at'])

    def test_conditional_304_preserves_original_body_and_validation(self):
        fetch = unittest.mock.Mock(side_effect=[(200, {'etag': 'v1'}, b'[{}]'), (304, {'etag': 'v1'}, b'')])
        one = self.transport('one', fetch).get(URL, self.options)
        two = self.transport('two', fetch).get(URL, {**self.options, 'revalidate_after_seconds': 0})
        self.assertEqual(fetch.call_args.args[1]['If-None-Match'], 'v1')
        self.assertEqual(one['body'], two['body'])
        self.assertEqual(one['body_collected_at'], two['body_collected_at'])
        self.assertEqual(two['validation']['http_status'], 304)
        self.assertNotEqual(two['body'], two['validation']['response'])

    def test_last_modified_and_changed_body_keep_both_versions(self):
        fetch = unittest.mock.Mock(side_effect=[(200, {'last-modified': 'Wed, 01 Jan 2025 00:00:00 GMT'}, b'[1]'), (200, {}, b'[2]')])
        one = self.transport('one', fetch).get(URL, self.options)
        transport = self.transport('two', fetch)
        two = transport.get(URL, {**self.options, 'revalidate_after_seconds': 0})
        self.assertIn('If-Modified-Since', fetch.call_args.args[1])
        self.assertNotEqual(one['body'], two['body'])
        self.assertEqual(transport.body(one['body']), b'[1]')
        self.assertEqual(transport.body(two['body']), b'[2]')

    def test_retry_after_and_immutable_failed_attempts(self):
        fetch = unittest.mock.Mock(side_effect=[(429, {'retry-after': '3'}, b'try later'), (200, {}, b'[]')])
        transport = self.transport('one', fetch)
        waits = []
        transport.wait = waits.append
        transport.get(URL, self.options)
        self.assertIn(3, waits)
        attempts = list((self.root/'one/requests').glob('*/[0-9]*/response.json'))
        self.assertEqual(len(attempts), 2)
        self.assertIn(429, [ingest.load(p)['http_status'] for p in attempts])

    def test_incomplete_response_retains_available_bytes_without_caching_success(self):
        fetch = unittest.mock.Mock(side_effect=[ingest.CaptureFailure('Incomplete body', 200, {'content-length': '10'}, b'[1'), (200, {}, b'[]')])
        transport = self.transport('one', fetch)
        result = transport.get(URL, self.options)
        first = ingest.load(self.root/'one/requests'/ingest.digest(URL.encode())/'1/response.json')
        self.assertEqual(transport.body(first['response']), b'[1')
        self.assertEqual(first['error'], 'Incomplete body')
        self.assertEqual(transport.body(result['body']), b'[]')

    def test_retry_budget_and_server_delay_bounds(self):
        fetch = unittest.mock.Mock(return_value=(503, {}, b'unavailable'))
        with self.assertRaisesRegex(RuntimeError, 'budget'):
            self.transport('one', fetch, maximum=2).get(URL, self.options)
        self.assertEqual(fetch.call_count, 2)
        with self.assertRaisesRegex(RuntimeError, 'Retry-After'):
            self.transport('two', lambda *_: (429, {'retry-after': '1000'}, b'')).get(URL, self.options)

    def test_interrupted_and_bad_json_requests_are_retained_on_resume(self):
        folder = self.root/'one/requests'/ingest.digest(URL.encode())/'1'
        ingest.save(folder/'request.json', {'url': URL, 'interrupted': True}, immutable=True)
        fetch = unittest.mock.Mock(side_effect=[(200, {}, b'broken'), (200, {}, b'[]')])
        self.transport('one', fetch).get(URL, self.options)
        self.assertTrue((folder.parent/'2/invalid.json').exists())
        self.assertTrue((folder.parent/'3/response.json').exists())
        self.assertEqual(ingest.load(folder/'request.json')['interrupted'], True)

    def test_corrupt_cache_rejected_even_when_fresh(self):
        transport = self.transport('one', lambda *_: (200, {}, b'[]'))
        ref = transport.get(URL, self.options)['body']
        (transport.root/'blobs'/ref['sha256']).write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'Corrupted'):
            self.transport('two', lambda *_: self.fail('Refetched corruption')).get(URL, self.options)

    def test_concurrent_sources_deduplicate_identical_capture_bytes(self):
        barrier = threading.Barrier(4)
        link = os.link
        def simultaneous_link(source, target):
            barrier.wait(timeout=5)
            link(source, target)
        target = self.root/'shared-empty-body'
        with patch.object(ingest.os, 'link', side_effect=simultaneous_link), ingest.concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            list(workers.map(lambda _: ingest.save(target, b'[]', immutable=True), range(4)))
        self.assertEqual(target.read_bytes(), b'[]')
        with self.assertRaises(ValueError):
            ingest.save(target, b'different', immutable=True)

    def test_pagination_overfetch_and_incremental_overlap(self):
        config = self.config(mode='incremental', page_size=2)
        source = config['sources'][0]
        plan = ingest.source_plan(config, source, '2025-01-20', dt.date(2025, 1, 25))
        self.assertEqual(plan['from'], '2025-01-14')
        self.assertEqual(plan['to'], '2025-01-25')
        fetch = unittest.mock.Mock(side_effect=[(200, {}, json.dumps([article(1), article(2)]).encode()), (200, {}, b'[]')])
        manifest = ingest.acquire(self.transport('one', fetch), source, plan)
        self.assertEqual(len(manifest['pages']), 2)
        self.assertIn('after=2025-01-13T00%3A00%3A00', manifest['pages'][0]['url'])

    def test_store_lock_and_interrupt(self):
        with ingest.exclusive(self.root/'lock'):
            with self.assertRaisesRegex(RuntimeError, 'active'):
                with ingest.exclusive(self.root/'lock'):
                    self.fail('Acquired duplicate lock')
        ingest.STOP.set()
        with self.assertRaises(InterruptedError):
            self.transport('one', lambda *_: self.fail('Fetched after stop')).get(URL, self.options)

    def test_watch_reuses_failed_job_and_stops_cleanly(self):
        self.config()
        raw = ingest.load(self.root/'config.json')
        raw['schedule'] = {'every_seconds': 1}
        ingest.save(self.root/'config.json', raw)
        calls = []
        def fake(config, native, resume):
            calls.append(resume)
            if len(calls) == 1:
                active = Path(config['store'])/'ingestion/active'/f'{ingest.digest(ingest.encoded(config))}.json'
                ingest.save(active, {'job': 'same-job'})
                raise RuntimeError('fixture interruption')
            ingest.STOP.set()
            return {'state': 'COMPLETED'}
        with patch.object(ingest, 'run_job', side_effect=fake), patch.object(ingest.signal, 'signal'), context_stdout():
            self.assertEqual(ingest.main(['--native', '/fixture', '--config', str(self.root/'config.json'), '--watch']), 0)
        self.assertEqual(calls, [None, 'same-job'])

    def test_empty_feed_verifies_without_model_preparation(self):
        config = self.config()
        config['processing']['prepare'] = True
        real_transport = ingest.Transport
        phases = []
        def phase(binary, store, job, name, args):
            phases.append(name)
            value = {'retained_reports': 0, 'collection': [{'coverage': [{'stop_reason': 'EXHAUSTED', 'provider_totals_changed': False, 'to': '2025-01-31'}]}]}
            return {'stdout': json.dumps(value if name.startswith('import-') else {'state': 'NOT_PREPARED'})}
        with patch.object(ingest, 'Transport', side_effect=lambda root, job, maximum: real_transport(root, job, maximum, lambda *_: (200, {}, b'[]'))), patch.object(ingest, 'native_phase', side_effect=phase), context_stdout():
            result = ingest.run_job(config, '/unused')
        self.assertEqual(phases, ['import-fixture', 'verify', 'status'])
        self.assertEqual(result['preparation_status'], 'NO_REPORTS')
        self.assertFalse(result['prepared'])

    def test_processing_failure_does_not_advance_source_checkpoint(self):
        config = self.config(mode='incremental')
        policy = self.root/'policy.json'
        ingest.save(policy, {})
        config['processing'] = {'prepare': True, 'config': str(policy), 'attempt': 'initial', 'policy_sha256': ingest.digest(policy.read_bytes())}
        real_transport = ingest.Transport
        def phase(binary, store, job, name, args):
            value = {'retained_reports': 1, 'collection': [{'coverage': [{'stop_reason': 'EXHAUSTED', 'provider_totals_changed': False, 'to': '2025-01-31'}]}]}
            return {'stdout': json.dumps(value if name.startswith('import-') else {'reports_failed': 1})}
        with patch.object(ingest, 'Transport', side_effect=lambda root, job, maximum: real_transport(root, job, maximum, lambda *_: (200, {}, json.dumps([article()]).encode()))), patch.object(ingest, 'native_phase', side_effect=phase), context_stdout():
            with self.assertRaisesRegex(RuntimeError, 'processing failures'):
                ingest.run_job(config, '/unused')
        self.assertFalse((Path(config['store'])/'ingestion/checkpoints').exists())
        self.assertEqual(len(list((Path(config['store'])/'ingestion/jobs').glob('*/last-error.json'))), 1)

    def test_real_http_conditional_get_and_redirect_retention(self):
        requests = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                requests.append(dict(self.headers))
                if self.path == '/redirect':
                    self.send_response(302)
                    self.send_header('Location', '/posts')
                elif self.headers.get('If-None-Match') == 'fixture-v1':
                    self.send_response(304)
                else:
                    self.send_response(200)
                self.send_header('ETag', 'fixture-v1')
                if self.path == '/truncated':
                    self.send_header('Content-Length', '100')
                self.end_headers()
                if self.path in ('/posts', '/truncated') and not self.headers.get('If-None-Match'):
                    self.wfile.write(b'[]')
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f'http://127.0.0.1:{server.server_port}'
            first = self.transport('one', ingest.download).get(url+'/posts', self.options)
            second = self.transport('two', ingest.download).get(url+'/posts', {**self.options, 'revalidate_after_seconds': 0})
            self.assertEqual(second['cache'], 'revalidated')
            self.assertEqual(second['body'], first['body'])
            with self.assertRaisesRegex(RuntimeError, '302'):
                self.transport('three', ingest.download).get(url+'/redirect', self.options)
            self.assertEqual(len(requests), 3, 'Redirect must not silently fetch another host/path')
            with self.assertRaisesRegex(RuntimeError, 'Content-Length'):
                self.transport('four', ingest.download).get(url+'/truncated', {**self.options, 'max_attempts': 1})
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    @unittest.skipUnless(os.environ.get('AXTHENA_NATIVE'), 'Set AXTHENA_NATIVE for actual C++/Neo4j integration')
    def test_native_import_dedup_version_resume_and_corruption(self):
        binary = os.environ['AXTHENA_NATIVE']
        config = self.config()
        real_transport = ingest.Transport
        client = self.root/'bin/opencode'
        client.parent.mkdir()
        client.write_bytes((COMPONENT/'tests/ingestion_model_fixture.py').read_bytes())
        client.chmod(0o700)
        policy = ingest.load(COMPONENT/'config/ingestion-processing-v1.json')
        policy['model'] = {'providerID': 'fixture', 'id': 'offline-ingestion'}
        policy_path = self.root/'policy.json'
        ingest.save(policy_path, policy)
        config['processing'] = {'prepare': True, 'config': str(policy_path), 'attempt': 'initial',
                                'policy_sha256': ingest.digest(policy_path.read_bytes())}
        network = unittest.mock.Mock(return_value=(200, {'etag': 'v1'}, json.dumps([article()]).encode()))
        def transport(root, job, maximum):
            return real_transport(root, job, maximum, network, wait=lambda _: None)
        store = Path(config['store'])
        try:
            with patch.object(ingest, 'Transport', side_effect=transport), patch.dict(os.environ, {'PATH': str(client.parent)+os.pathsep+os.environ['PATH']}), context_stdout():
                first = ingest.run_job(config, binary)
                self.assertEqual(first['collection'][0]['retained_reports'], 1)
                self.assertTrue(first['prepared'])
                self.assertEqual(first['store_status']['state'], 'PREPARED')
                self.assertEqual(first['store_status']['reports_failed'], 0)
                resumed = ingest.run_job(config, binary, first['job'])
                self.assertEqual(resumed, first)
                second = ingest.run_job(config, binary)
            self.assertEqual(network.call_count, 1)
            self.assertEqual(second['collection'][0]['retained_reports'], 1)
            self.assertEqual(second['collection'][0]['observations'], 1)
            checkpoint = store/'ingestion/checkpoints'/f'{ingest.digest(ingest.encoded(config))}.json'
            self.assertEqual(ingest.load(checkpoint), {'fixture': '2025-01-31'})
            # Revisited windows retain changed data without replacing selected reports.
            config['sources'][0]['fetch']['revalidate_after_seconds'] = 0
            network.return_value = (200, {'etag': 'v2'}, json.dumps([article(body='Revised body')]).encode())
            with patch.object(ingest, 'Transport', side_effect=transport), patch.dict(os.environ, {'PATH': str(client.parent)+os.pathsep+os.environ['PATH']}), context_stdout():
                third = ingest.run_job(config, binary)
            self.assertEqual(third['collection'][0]['retained_reports'], 1)
            self.assertEqual(third['collection'][0]['observations'], 2)
            network.return_value = (304, {'etag': 'v2'}, b'')
            with patch.object(ingest, 'Transport', side_effect=transport), patch.dict(os.environ, {'PATH': str(client.parent)+os.pathsep+os.environ['PATH']}), context_stdout():
                revalidated = ingest.run_job(config, binary)
            self.assertEqual(revalidated['collection'][0]['retained_reports'], 1)
            self.assertEqual(revalidated['collection'][0]['observations'], 3)
            # A network budget must not be charged again for native decoding of cached pages.
            config['collection']['page_size'] = 1
            config['sources'][0]['fetch']['revalidate_after_seconds'] = 21600
            network.side_effect = lambda url, *_: (200, {}, json.dumps([article()] if '&page=1&' in url else []).encode())
            with patch.object(ingest, 'Transport', side_effect=transport), patch.dict(os.environ, {'PATH': str(client.parent)+os.pathsep+os.environ['PATH']}), context_stdout():
                ingest.run_job(config, binary)
                requests_before = network.call_count
                config['collection']['max_requests'] = 1
                cached_pages = ingest.run_job(config, binary)
            self.assertEqual(network.call_count, requests_before)
            self.assertEqual(len(cached_pages['collection'][0]['collection'][0]['pages']), 2)
            subprocess.run([binary, 'verify', '--store', str(store)], check=True, capture_output=True)
            # A manifest with a wrong body digest must be rejected by C++, not trusted.
            manifest_path = store/'ingestion/jobs'/third['job']/'captures-fixture.json'
            bad = ingest.load(manifest_path)
            bad['pages'][0]['body']['sha256'] = '0'*64
            ingest.save(self.root/'bad.json', bad)
            rejected = subprocess.run([binary, 'import-captures', '--manifest', str(self.root/'bad.json'), '--store', str(store)], capture_output=True)
            self.assertNotEqual(rejected.returncode, 0)
        finally:
            marker = store/'neo4j-store.json'
            if marker.exists():
                connection = ingest.load(COMPONENT/'artifacts/neo4j/connection.json')
                namespace = ingest.load(marker)['namespace']
                password = Path(connection['password_file']).read_text()
                auth = base64.b64encode(f"{connection['user']}:{password}".encode()).decode()
                query = {'statement': 'MATCH (n) WHERE ((n:AxRecord OR n:AxRun OR n:AxDocument) AND n.store=$space) OR (n:AxStore AND n.id=$space) DETACH DELETE n RETURN count(n) AS removed', 'parameters': {'space': namespace}}
                req = urllib.request.Request(f"{connection['endpoint']}/db/{connection['database']}/query/v2", data=json.dumps(query).encode(), headers={'Content-Type': 'application/json', 'Authorization': 'Basic '+auth})
                with urllib.request.urlopen(req) as response:
                    self.assertFalse(json.load(response).get('errors'))


def context_stdout():
    return unittest.mock.patch('sys.stdout', new_callable=io.StringIO)


if __name__ == '__main__':
    unittest.main()
