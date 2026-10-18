# V2 starting point and roadmap

**Proposed milestones, starting from the delivered V1.** No delivery dates or
performance targets are promised by this plan.

## Preserve the baseline

- C++ owns identity, validation, graph publication and query semantics.
- Python owns source adapters, HTTP/cache and job orchestration; Neo4j is authoritative.
- Source/model attempts and published runs remain immutable, with explicit lineage.
- Similarity retrieves candidates; validated SAME decisions alone permit category reuse.
- Strict history requires exact-version availability; declared dates cannot substitute.
- Run, coverage, exclusions and truncation remain visible in every investigation.

## Recommended sequence

| Milestone | Deliverable | Acceptance evidence |
|---|---|---|
| Baseline first | Frozen workloads, semantic labels and raw performance samples | Repeatable result hashes, environment, failures and reviewed labels |
| Paired-period investigation | Two independently scoped windows over one pinned run | Correct report intersections, per-side coverage, empty/partial views, saved comparison |
| Decision inspection | Domain/identity candidate, retry and evidence panels | Original labels and decisions inspectable, including non-merges |
| Operations visibility | Ingestion status and failure drill-down | Resume identity, cache lineage and explicit partial/failure states |
| Selected storage work | Recoverable compression | Exact round trip, old archive readability, corruption rejection and restore parity |
| Selected historical work | Market-session-aligned pictures | Explicit timezone/calendar/cutoff rules and unknown-availability exclusion |
| Measured scale | Optimize an observed bottleneck | Same-result before/after measurements, memory and failure behavior |
| Release review | Reproducible demo and versioned verification packet | Build/runtime provenance, semantic review, benchmarks and recovery procedure |

The first recommended task is reproducing existing reads on a restored namespace,
recording the [benchmark baseline](benchmarking.md), and reviewing domain reuses,
sampled non-merges and partial reports. The first product candidate is paired-period
comparison with independent coverage and linked source drill-down.

New API/save formats, codecs, market calendars, concurrency behavior and data scopes
need explicit contracts before implementation. A roadmap item does not imply that
its feature exists or that a broader earlier requirement has been closed.

Quoted-speaker identity, active source revisions, adaptive retrieval, generalized
confirmation authorities and predictive/simulation work remain research topics.
