#!/usr/bin/env python3
"""Local resumable ingestion. Python owns fetching; the native CLI owns the store."""
import argparse
import concurrent.futures
import contextlib
import datetime as dt
import email.utils
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
from urllib.parse import urlsplit

from ingestors import ADAPTERS, windows

STOP = threading.Event()
MAX_BODY = 32 * 1024 * 1024


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False) + '\n').encode()


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec='seconds').replace('+00:00', 'Z')


def load(path):
    return json.loads(Path(path).read_bytes())


def save(path, value, immutable=False):
    path = Path(path)
    data = value if isinstance(value, bytes) else encoded(value)
    path.parent.mkdir(parents=True, exist_ok=True)
    if immutable and path.exists():
        if path.read_bytes() != data:
            raise ValueError(f'Immutable artifact differs: {path}')
        return
    temporary = path.with_name('.pending-' + uuid.uuid4().hex)
    try:
        with temporary.open('xb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if immutable:
            try:
                os.link(temporary, path)
            except FileExistsError:
                # Concurrent sources may return identical bytes (especially [] pages).
                if path.read_bytes() != data:
                    raise ValueError(f'Immutable artifact differs: {path}') from None
        else:
            os.replace(temporary, path)
        fd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    finally:
        temporary.unlink(missing_ok=True)


@contextlib.contextmanager
def exclusive(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('Another ingestion job is active in this store') from None
        yield


def keys(value, allowed, required=()):
    if not isinstance(value, dict) or set(value) - set(allowed) or set(required) - set(value):
        raise ValueError(f'Invalid configuration keys; expected {sorted(allowed)}, required {sorted(required)}')


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'Expected integer in [{low}, {high}], got {value!r}')


FETCH = {'timeout_seconds': 30, 'interval_seconds': 1, 'concurrency': 2,
         'max_attempts': 3, 'retry_base_seconds': 2, 'retry_max_seconds': 120,
         'revalidate_after_seconds': 21600, 'conditional_requests': True}


def fetch_config(raw):
    keys(raw, FETCH)
    value = {**FETCH, **raw}
    for name, low, high in [('timeout_seconds', 1, 300), ('interval_seconds', 0, 3600),
                            ('concurrency', 1, 8), ('max_attempts', 1, 5),
                            ('retry_base_seconds', 0, 300), ('retry_max_seconds', 1, 3600),
                            ('revalidate_after_seconds', 0, 86400)]:
        integer(value[name], low, high)
    if type(value['conditional_requests']) is not bool:
        raise ValueError('conditional_requests must be boolean')
    return value


def configuration(path):
    path = Path(path).resolve()
    value = load(path)
    keys(value, ['schema_version', 'store', 'schedule', 'collection', 'fetch', 'sources', 'processing'],
         ['schema_version', 'collection', 'sources'])
    if value['schema_version'] != 'axthena.ingestion.v1':
        raise ValueError('Unsupported ingestion configuration')
    collection = value['collection']
    keys(collection, ['mode', 'from', 'to', 'lookback_days', 'partition', 'page_size', 'articles_per_scope', 'pages_per_scope', 'max_requests'],
         ['from', 'to', 'articles_per_scope', 'pages_per_scope', 'max_requests'])
    collection = {'mode': 'backfill', 'lookback_days': 7, 'partition': 'month', 'page_size': 10, **collection}
    start, end = dt.date.fromisoformat(collection['from']), dt.date.fromisoformat(collection['to'])
    if start > end or (end-start).days > 36600 or start.year < 1900 or end.year > 9998:
        raise ValueError('Invalid bounded collection date range')
    if collection['mode'] not in ('backfill', 'incremental') or collection['partition'] not in ('month', 'window'):
        raise ValueError('Invalid collection mode or partition')
    for key, low, high in [('lookback_days', 1, 366), ('page_size', 1, 100), ('articles_per_scope', 1, 100000),
                           ('pages_per_scope', 1, 10000), ('max_requests', 1, 100000)]:
        integer(collection[key], low, high)
    value['collection'] = collection
    value['fetch'] = fetch_config(value.get('fetch', {}))
    schedule = value.setdefault('schedule', {'every_seconds': 21600})
    keys(schedule, ['every_seconds'], ['every_seconds'])
    integer(schedule['every_seconds'], 1, 604800)
    sources = value['sources']
    if not isinstance(sources, list) or not 1 <= len(sources) <= 100:
        raise ValueError('Expected 1–100 explicit sources')
    ids, endpoints = set(), set()
    for source in sources:
        keys(source, ['id', 'ingestor', 'endpoint', 'name', 'search', 'fetch'], ['id', 'ingestor', 'endpoint'])
        if not isinstance(source['id'], str) or not re.fullmatch('[a-z0-9][a-z0-9_-]{0,63}', source['id']) or source['id'] in ids:
            raise ValueError('Source IDs must be unique safe names')
        ids.add(source['id'])
        if source['ingestor'] not in ADAPTERS:
            raise ValueError(f"Unknown ingestor: {source['ingestor']}")
        url = urlsplit(source['endpoint'])
        if url.scheme != 'https' or not re.fullmatch(r'[A-Za-z0-9.-]+', url.netloc) or url.query or url.fragment or source['endpoint'] in endpoints:
            raise ValueError('Source endpoints must be unique public HTTPS URLs without query/credentials')
        endpoints.add(source['endpoint'])
        source.setdefault('name', source['id'])
        if not isinstance(source['name'], str) or not isinstance(source.get('search', ''), str):
            raise ValueError('Source name/search must be strings')
        overrides = source.get('fetch', {})
        if 'concurrency' in overrides:
            raise ValueError('concurrency is global; each source is paginated sequentially')
        source['fetch'] = fetch_config({**value['fetch'], **overrides})
    processing = value.setdefault('processing', {})
    keys(processing, ['prepare', 'config', 'attempt'], [])
    processing.setdefault('prepare', True)
    processing.setdefault('attempt', 'initial')
    if type(processing['prepare']) is not bool or not isinstance(processing['attempt'], str):
        raise ValueError('Invalid processing options')
    if processing['prepare']:
        policy = Path(processing.get('config', 'application-v1.json'))
        policy = policy if policy.is_absolute() else path.parent / policy
        processing['config'] = str(policy.resolve(strict=True))
        processing['policy_sha256'] = digest(policy.read_bytes())
    store = Path(value.get('store', '../artifacts/neo4j-v1/store'))
    value['store'] = str((store if store.is_absolute() else path.parent/store).resolve())
    return value


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


class CaptureFailure(Exception):
    def __init__(self, message, status, headers, body):
        super().__init__(message)
        self.status, self.headers, self.body = status, headers, body


def download(url, headers, timeout):
    opener = urllib.request.build_opener(NoRedirect())
    request = urllib.request.Request(url, headers=headers)
    try:
        response = opener.open(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        response_headers = {k.lower(): v for k, v in response.headers.items()}
        try:
            body = response.read(MAX_BODY + 1)
        except Exception as error:
            raise CaptureFailure(str(error), response.code, response_headers, getattr(error, 'partial', b'')[:MAX_BODY]) from error
        if len(body) > MAX_BODY:
            raise CaptureFailure('Response exceeds 32 MiB bound; retained prefix', response.code, response_headers, body[:MAX_BODY])
        expected = response_headers.get('content-length')
        if response.code != 304 and expected is not None and (not expected.isdigit() or int(expected) != len(body)):
            raise CaptureFailure('Incomplete or invalid Content-Length', response.code, response_headers, body)
        return response.code, response_headers, body


class Transport:
    def __init__(self, root, job, max_requests, fetch=download, wait=None):
        self.root, self.job = Path(root), Path(job)
        self.fetch = fetch
        self.wait = wait or self.pause
        self.remaining = max_requests
        self.guard = threading.Lock()
        self.last_request = {}
        self.host_locks = {}

    @staticmethod
    def pause(seconds):
        if STOP.wait(seconds):
            raise InterruptedError('Ingestion stopped')

    def blob(self, body):
        key = digest(body)
        save(self.root/'blobs'/key, body, immutable=True)
        return {'sha256': key, 'bytes': len(body)}

    def body(self, ref):
        if not re.fullmatch('[0-9a-f]{64}', ref['sha256']):
            raise ValueError('Invalid capture hash')
        data = (self.root/'blobs'/ref['sha256']).read_bytes()
        if len(data) != ref['bytes'] or digest(data) != ref['sha256']:
            raise ValueError('Corrupted cached capture')
        return data

    def get(self, url, options):
        key = digest(url.encode())
        folder = self.job/'requests'/key
        success = folder/'selected.json'
        if success.exists():
            result = load(success)
            self.body(result['body'])
            return result
        cache_path = self.root/'cache'/f'{key}.json'
        cached = load(cache_path) if cache_path.exists() else None
        current = time.time()
        if cached:
            if cached['url'] != url:
                raise ValueError('Cache identity mismatch')
            self.body(cached['body'])
            if 0 <= current - cached['checked_epoch'] < options['revalidate_after_seconds']:
                result = {**cached, 'cache': 'fresh', 'observed_at': stamp()}
                save(success, result, immutable=True)
                return result
        headers = {'User-Agent': 'Axthena/0.3 local research collector', 'Accept': 'application/json'}
        if cached and options['conditional_requests']:
            if cached['headers'].get('etag'):
                headers['If-None-Match'] = cached['headers']['etag']
            elif cached['headers'].get('last-modified'):
                headers['If-Modified-Since'] = cached['headers']['last-modified']
        folder.mkdir(parents=True, exist_ok=True)
        sequence = max([int(p.name) for p in folder.iterdir() if p.is_dir() and p.name.isdigit()] + [0])
        for attempt in range(options['max_attempts']):
            if STOP.is_set():
                raise InterruptedError('Ingestion stopped')
            with self.guard:
                if self.remaining <= 0:
                    raise RuntimeError('Global HTTP request budget exhausted; resume with the same job')
                self.remaining -= 1
            host = urlsplit(url).netloc
            directory = folder/str(sequence+attempt+1)
            request = {'url': url, 'headers': headers, 'started_at': stamp()}
            save(directory/'request.json', request, immutable=True)
            status, response_headers, body, error = 0, {}, b'', None
            try:
                with self.guard:
                    host_lock = self.host_locks.setdefault(host, threading.Lock())
                with host_lock:
                    self.wait(max(0, options['interval_seconds'] - (time.monotonic()-self.last_request.get(host, 0))))
                    self.last_request[host] = time.monotonic()
                    status, response_headers, body = self.fetch(url, headers, options['timeout_seconds'])
            except InterruptedError:
                raise
            except CaptureFailure as exc:
                status, response_headers, body, error = exc.status, exc.headers, exc.body, str(exc)
            except Exception as exc:
                error = str(exc)
            receipt = {'url': url, 'http_status': status, 'headers': response_headers,
                       'response': self.blob(body), 'observed_at': stamp(), 'error': error,
                       'request': self.blob(encoded(request))}
            save(directory/'response.json', receipt, immutable=True)
            if status == 200 and error is None:
                try:
                    if not isinstance(json.loads(body), list):
                        raise ValueError('Publisher response must be an array')
                except (ValueError, UnicodeError) as exc:
                    save(directory/'invalid.json', {'error': str(exc)}, immutable=True)
                    error = str(exc)
            if (status == 200 and error is None) or (status == 304 and cached is not None and len(headers) > 2):
                if status == 304:
                    effective = {**cached, 'headers': {**cached['headers'], **response_headers}}
                else:
                    effective = {'url': url, 'body': receipt['response'], 'headers': response_headers,
                                 'body_collected_at': receipt['observed_at'], 'origin': receipt}
                result = {**effective, 'checked_epoch': time.time(), 'observed_at': receipt['observed_at'],
                          'cache': 'revalidated' if status == 304 else 'downloaded', 'validation': receipt}
                save(cache_path, result)
                save(success, result, immutable=True)
                return result
            retryable = error is not None or status in (408, 429, 500, 502, 503, 504)
            if not retryable or attempt+1 == options['max_attempts']:
                raise RuntimeError(f'Fetch failed ({status}): {url}: {error or "HTTP error"}')
            delay = min(options['retry_max_seconds'], options['retry_base_seconds'] * 2**attempt)
            retry_after = response_headers.get('retry-after')
            if retry_after:
                try:
                    delay = max(delay, float(retry_after) if retry_after.isdigit() else email.utils.parsedate_to_datetime(retry_after).timestamp()-time.time())
                except (ValueError, TypeError):
                    pass
            # A server-requested delay outside our bound stops rather than retrying too early.
            if delay > options['retry_max_seconds']:
                raise RuntimeError('Retry-After exceeds this job wait bound; retry later')
            self.wait(delay)


def source_plan(config, source, checkpoint=None, today=None):
    collection = config['collection']
    start, end = dt.date.fromisoformat(collection['from']), dt.date.fromisoformat(collection['to'])
    if collection['mode'] == 'incremental':
        end = min(end, today or dt.datetime.now(dt.timezone.utc).date())
        if checkpoint:
            start = max(start, dt.date.fromisoformat(checkpoint) - dt.timedelta(days=collection['lookback_days']-1))
    if start > end:
        return None
    provider = {k: source[k] for k in ('name', 'endpoint', 'search') if k in source}
    return {'schema_version': 'axthena.public-plan.v1', 'from': start.isoformat(), 'to': end.isoformat(),
            **{k: collection[k] for k in ('partition', 'page_size', 'articles_per_scope', 'pages_per_scope', 'max_requests')},
            'sources': [provider]}


def acquire(transport, source, plan):
    adapter = ADAPTERS[source['ingestor']]
    pages = []
    for start, end in windows(plan):
        selected, seen = set(), set()
        for page in range(1, plan['pages_per_scope']+1):
            url = adapter.url(source, start, end, page, plan['page_size'])
            result = transport.get(url, source['fetch'])
            pages.append(result)
            data = transport.body(result['body'])
            items = json.loads(data)
            if len(items) > plan['page_size']:
                raise ValueError('Publisher page exceeds configured size')
            if digest(data) in seen or not items:
                break
            seen.add(digest(data))
            selected.update(adapter.selected_ids(items, source, start, end))
            total_pages = result['headers'].get('x-wp-totalpages')
            if len(selected) >= plan['articles_per_scope'] or len(items) < plan['page_size'] or (total_pages is not None and int(total_pages) == page):
                break
    return {'schema_version': 'axthena.captures.v1', 'plan': plan, 'pages': pages}


def native_phase(binary, store, job, name, args):
    if STOP.is_set():
        raise InterruptedError('Stopped before native phase')
    receipt = job/f'{name}.json'
    if receipt.exists():
        return load(receipt)
    attempt = job/'native'/f'{name}-{uuid.uuid4().hex}'
    attempt.mkdir(parents=True)
    command = [str(binary), *args, '--store', str(store)]
    save(attempt/'request.json', {'command': command, 'started_at': stamp()}, immutable=True)
    with (attempt/'stdout').open('xb') as out, (attempt/'stderr').open('xb') as err:
        process = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True)
        try:
            while process.poll() is None:
                if STOP.wait(.2):
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        with contextlib.suppress(ProcessLookupError):
                            os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                    raise InterruptedError('Stopped during native phase; retained attempts may require an explicit model retry')
        finally:
            save(attempt/'completion.json', {'exit_code': process.poll(), 'finished_at': stamp()}, immutable=True)
    if process.returncode:
        raise RuntimeError(f'Native {name} failed; see {attempt}/stderr')
    result = {'command': command, 'stdout': (attempt/'stdout').read_text(), 'finished_at': stamp()}
    save(receipt, result, immutable=True)
    return result


def run_job(config, binary, resume=None):
    store = Path(config['store'])
    root = store/'ingestion'
    with exclusive(root/'operation.lock'):
        config_id = digest(encoded(config))
        state_path = root/'checkpoints'/f'{config_id}.json'
        active_path = root/'active'/f'{config_id}.json'
        checkpoints = load(state_path) if state_path.exists() else {}
        if resume:
            if not re.fullmatch('[a-zA-Z0-9_-]{1,80}', resume):
                raise ValueError('Invalid job ID')
            job = root/'jobs'/resume
            frozen = load(job/'job.json')
            if frozen['config'] != config:
                raise ValueError('Resume configuration differs from frozen job')
            if (job/'completed.json').exists():
                if active_path.exists() and load(active_path)['job'] == job.name:
                    active_path.unlink()
                return load(job/'completed.json')
        else:
            job = root/'jobs'/('job-'+uuid.uuid4().hex)
            frozen = {'schema_version': 'axthena.ingestion-job.v1', 'config': config, 'started_at': stamp(),
                      'plans': {s['id']: source_plan(config, s, checkpoints.get(s['id'])) for s in config['sources']}}
            save(job/'job.json', frozen, immutable=True)
        save(active_path, {'job': job.name})
        print(json.dumps({'job': job.name, 'state': 'RUNNING'}), flush=True)
        try:
            transport = Transport(root, job, config['collection']['max_requests'])
            sources = [s for s in config['sources'] if frozen['plans'][s['id']] is not None]
            def capture(source):
                path = job/f"captures-{source['id']}.json"
                if not path.exists():
                    manifest = acquire(transport, source, frozen['plans'][source['id']])
                    manifest['batch'] = job.name+'-'+source['id']
                    manifest['blob_root'] = str(root/'blobs')
                    return path, manifest
                return path, load(path)
            with concurrent.futures.ThreadPoolExecutor(max_workers=config['fetch']['concurrency']) as executor:
                captures = list(executor.map(capture, sources))
            manifests = []
            for path, manifest in captures:
                if not path.exists():
                    # Freeze all attempts only after source workers have completed.
                    manifest['attempts'] = [load(p) for p in sorted((job/'requests').glob('*/[0-9]*/response.json'))]
                    save(path, manifest, immutable=True)
                manifests.append(path)
            receipts = []
            updated = dict(checkpoints)
            for source, manifest in zip(sources, manifests):
                result = native_phase(binary, store, job, 'import-'+source['id'], ['import-captures', '--manifest', str(manifest)])
                summary = json.loads(result['stdout'])
                receipts.append(summary)
                coverage = summary['collection'][0]['coverage']
                if any(row['stop_reason'] not in ('EXHAUSTED', 'ARTICLE_LIMIT', 'PAGE_LIMIT') for row in coverage):
                    raise RuntimeError('Native collection validation found incomplete/invalid captures; inspect import receipt')
                # Do not advance past an earlier incomplete scope.
                for row in ([] if summary.get('issues') else coverage):
                    if row['stop_reason'] != 'EXHAUSTED' or row['provider_totals_changed']:
                        break
                    updated[source['id']] = max(updated.get(source['id'], ''), row['to'])
            has_reports = bool(receipts and receipts[-1]['retained_reports'])
            prepared = bool(has_reports and config['processing']['prepare'])
            if prepared:
                policy = Path(config['processing']['config'])
                if digest(policy.read_bytes()) != config['processing']['policy_sha256']:
                    raise ValueError('Preparation policy changed during job')
                native_phase(binary, store, job, 'prepare', ['prepare', '--config', str(policy), '--attempt', config['processing']['attempt']])
            if sources:
                native_phase(binary, store, job, 'verify', ['verify'])
            status = native_phase(binary, store, job, 'status', ['status']) if sources else None
            store_status = json.loads(status['stdout']) if status else None
            if prepared and store_status.get('reports_failed', 0):
                raise RuntimeError('Published native run contains whole-report processing failures; checkpoint withheld; use explicit model recovery')
            result = {'job': job.name, 'state': 'COMPLETED', 'finished_at': stamp(), 'collection': receipts,
                      'prepared': prepared,
                      'preparation_status': 'PUBLISHED' if prepared else 'DISABLED' if not config['processing']['prepare'] else 'NO_REPORTS',
                      'store_status': store_status}
            save(state_path, updated)
            save(job/'completed.json', result, immutable=True)
            active_path.unlink(missing_ok=True)
            return result
        except Exception as exc:
            save(job/'last-error.json', {'error': str(exc), 'at': stamp()})
            raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    parser.add_argument('--native', required=True, help=argparse.SUPPRESS)
    parser.add_argument('--store')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--resume')
    parser.add_argument('--watch', action='store_true')
    args = parser.parse_args(argv)
    if args.resume and args.watch:
        parser.error('--resume is a one-shot operation')
    config = configuration(args.config)
    if args.store:
        config['store'] = str(Path(args.store).resolve())
    if args.dry_run:
        print(json.dumps({'state': 'DRY_RUN', 'config': config,
                          'plans': {s['id']: source_plan(config, s) for s in config['sources']}}, indent=2))
        return 0
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: STOP.set())
    while not STOP.is_set():
        started = time.monotonic()
        try:
            active_path = Path(config['store'])/'ingestion'/'active'/f'{digest(encoded(config))}.json'
            resume = args.resume
            if args.watch and active_path.exists():
                resume = load(active_path)['job']
            print(json.dumps(run_job(config, args.native, resume), indent=2), flush=True)
        except Exception as exc:
            print(json.dumps({'state': 'STOPPED' if STOP.is_set() else 'FAILED', 'error': str(exc)}), file=sys.stderr, flush=True)
            if not args.watch:
                return 130 if STOP.is_set() else 1
        if not args.watch:
            break
        STOP.wait(max(0, config['schedule']['every_seconds']-(time.monotonic()-started)))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print(f'Axthena ingestion: {error}', file=sys.stderr)
        sys.exit(2)
