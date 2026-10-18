# Configurable ingestion

From the repository root, after the [native build](../README.md):

```sh
./axthena ingest --config config/ingestion-annual-2025.json --dry-run
./axthena ingest --config config/ingestion-annual-2025.json
./axthena ingest --config config/ingestion-annual-2025.json --resume job-REPLACE_WITH_PRINTED_ID
./axthena ingest --config config/ingestion-annual-2025.json --watch
```

Dry-run performs no source requests, database work or model calls. Watch is opt-in;
Ctrl-C or SIGTERM stops it. An OS scheduler can invoke the one-shot command instead.
Overlapping jobs in the same store are rejected.

## Configuration

[`ingestion-annual-2025.json`](../config/ingestion-annual-2025.json) selects the
2025 NVIDIA/Microsoft sample. Copy and edit it for another explicit scope.

| Section | Settings |
|---|---|
| `sources` | IDs, registered adapter, HTTPS endpoint, search and per-source fetch overrides |
| `collection` | Inclusive dates, backfill/incremental mode, partition, page/article/request limits |
| `fetch` | Concurrency, per-host delay, timeout, retries, backoff, freshness and conditional requests |
| `schedule.every_seconds` | Watch interval; jobs never overlap |
| `processing` | Preparation toggle, policy file and model-attempt version |
| `store` | Evidence/job directory and associated Neo4j namespace |

Config-relative paths resolve from the configuration file; CLI paths resolve from
the current directory. `--store PATH` overrides the configured store. Unknown fields
and invalid limits fail before acquisition. Provider credentials remain in OpenCode.

Set `processing.prepare` to `false` for collection/import without models. The
default processing policy enables name/vector candidate-union domain matching.
Only validated SAME decisions permit reuse; broader/narrower, related and uncertain
categories remain distinct. Original labels and attempts remain available.

The sample selects up to two reports per publisher/month, not exhaustive coverage.
Watching this fixed historical scope does not extend its date range. Incremental
mode revisits `lookback_days` (default seven), bounded by explicit dates and today.
Checkpoints advance only through contiguous exhausted windows after successful
processing; sampling limits and invalid items prevent coverage advancement.

## Evidence, cache and recovery

- Resume reuses successful pages within the frozen job exactly.
- Fresh cached responses avoid requests; stale entries use ETag/Last-Modified.
- HTTP 304 retains the empty response, conditional request, original body and times.
- Changed bytes create immutable SHA-256 blobs. C++ verifies captures, identity,
  selected dates and deterministic text projection before corpus application.
- Repeated publisher IDs do not create extra reports. Changed bodies remain
  unselected observations rather than overwriting the original selected version.
- Every HTTP attempt is retained, including failures and incomplete attempts.
  Budgets include retries per invocation; Retry-After respects configured bounds.
- Interrupted model attempts remain evidence. Explicit versioned retries preserve
  first responses; the scheduler does not silently replace them.
- Matching requests reuse retained answers; changed downstream context can require
  new decisions. Retained replay is not deterministic fresh generation.

`STORE/ingestion/jobs/JOB_ID/` retains frozen configuration, plans, requests,
manifests, phase logs and final receipt. `blobs/` is immutable; `cache/`, `active/`
and `checkpoints/` are mutable controls. Native publication copies the referenced
capture bytes into the evidence archive. Backup retains these files.

Resume requires the original frozen configuration and paths. After relocating a
store, start a new job with its new path rather than rewriting old receipts.
Watch retries a pending job before creating a new one; configuration reloads on
restart. Whole-report failures block checkpoint advancement even if a partial
graph was retained.

## Adapter boundary and verification

Python's standard library handles HTTP, cache, scheduling and adapters.
[`tools/ingestors.py`](../tools/ingestors.py) registers the WordPress adapter used
by both publishers. C++ validates `axthena.captures.v1` and owns identity,
preparation and Neo4j publication. A new provider format requires both an adapter
and a versioned native decoding contract with fixtures.

```sh
ctest --test-dir build/dev -R ingestion_orchestration --output-on-failure
```

Tests cover HTTP 304/redirects, cache/retry/resume and actual native import → prepare
→ verify using a deterministic offline model. Neo4j is required; publishers and live
model APIs are not contacted by these tests.
