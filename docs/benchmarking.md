# Benchmarking and evaluation

## Recorded development baseline

These are dated local measurements from the 48-report annual baseline before the
standalone domain pass. The original timing logs and live database are not included
in the public repository; the figures are historical, not a fresh-checkout benchmark.

| Measurement | Recorded result | Interpretation |
|---|---|---|
| Annual preparation replay | 25.86 s; peak RSS 1,006,780,416 bytes | Retained-answer replay, no fresh model inference |
| Nine distinct CLI reads | 84–1,245 ms | Different workloads, includes process/database/evidence/output costs |
| Five further warm stock-evidence reads | Median 1,150.81 ms; max 1,153.63 ms | Small local sample, not representative tail latency |
| Annual evidence archive | 17,533,544 bytes | Pre-domain archive, not total Neo4j storage |
| Later correctness suite | Eight CTest groups, 49.30 s | 205 native checks + 17 Python tests; not application throughput |
| Frontend | Five tests and browser checks | Correctness evidence, not a standardized FPS benchmark |

The runtime was macOS arm64, Neo4j Community 5.26.31, JDK 21 and the pinned Nomic
encoder. There is no accepted production SLA or representative semantic accuracy
score. A complete hardware/load manifest is needed for comparable new measurements.

## Reproducible protocol

1. Record commit/binary/config/prompt/encoder digests, machine/OS/compiler/runtime,
   explicit run/namespace, corpus manifest, requests and expected results.
2. Verify result/evidence correctness first. Use an independently restored store.
3. Separate direct queries, CLI startup, model generation, retained replay and UI
   rendering. Record concurrency, cache conditions and unavoidable background load.
4. For warm reads, use three warmups and 30 measured requests per case across three
   sessions. Rotate case order, retain failures and do not discard slow samples.
5. Time the entire operation with a monotonic clock, including reading response
   bytes. Report n, p50, nearest-rank p95, max, failure rate and per-session variation.
6. Retain returned IDs/counts, exclusions, truncation and response hashes. Faster
   output with missing records or evidence is not a valid optimization result.
7. Distinguish process restart, database restart and filesystem-cold conditions.
   Do not call a fresh process cold cache.

Suggested workloads: company overview, one-hop relationships, claims, exact evidence,
strict-empty history, company comparison, coverage, cache/304/resume and restore.
Record displayed node/edge counts and graph controls for UI trials. The 500-node
display cap is not a proven responsiveness target.

```sh
./axthena verify --store PATH_TO_PREPARED_STORE
ctest --preset axthena-dev --output-on-failure
./workspace/npmw test
```

These establish correctness, not a full timed workload. The recipe runner in
`scripts/` records request/response repeatability; its default pack requires the
historical run or adaptation to your own prepared corpus. A standardized timed
harness and synthetic-scale generator remain proposed.

## Semantic evaluation

Freeze held-out passages, candidate sets and reviewer labels before changing prompts.
Include equivalent, broader/narrower, related, negated, conditional, time-qualified
and ambiguous examples. Preserve disagreements and uncertain labels.

Measure candidate recall separately from false merges and missed equivalences.
Count malformed outputs, refusals, retries and retry-assisted successes separately.
Exact substring matching and citation-ID membership are structural checks; passage
support and interpretation quality require a separate semantic review.

Publish environment, requests, raw samples, failures, hashes and summary together.
Latency/memory/semantic thresholds and larger source scopes must be selected before
calling a result acceptable. See the [V2 roadmap](roadmap.md).
