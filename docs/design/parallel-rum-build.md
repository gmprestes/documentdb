# Parallel index builds for pg_documentdb_extended_rum

Status: DESIGN — implementation tracked on branch `feron-parallel-rum`.
Target: upstream PR to microsoft/documentdb once validated.

## Problem

Every regular DocumentDB index (single-field, compound, composite op-class)
is built by the `extended_rum` access method, and its build is single-core:
one backend scans the whole heap, accumulates entries in an in-memory
red-black tree bounded by `maintenance_work_mem`, and flushes sorted batches
into the index. On multi-GiB collections the build takes many minutes
(measured: 8.8M docs / 7.8GB ≈ 520s via CONCURRENTLY on 2 vCPU) while other
cores sit idle. B-tree got parallel builds in PG11; GIN — the AM RUM derives
from — got them in **PostgreSQL 18** (`ginbuild.c` parallel path). RUM never
did.

## Approach: port the PG18 parallel GIN build

PG18's parallel GIN build is structurally applicable because RUM shares GIN's
build shape (accumulate → sort → merge → write):

1. **Parallel heap scan.** Leader sets up `GinBuildShared` (our
   `RumBuildShared`) in DSM + a shared `tuplesort` (`SharedSortInfo`).
   Workers attach via `table_parallelscan_*`.
2. **Per-worker accumulation.** Each worker runs the existing
   `rumBuildCallback` path into its private `BuildAccumulator` (rbtree),
   flushing when `maintenance_work_mem / nparticipants` is hit — but instead
   of inserting into the index, it writes sorted `GinTuple`-style entries
   (key, category, packed TID list) into the shared tuplesort
   (`tuplesort_putgintuple` equivalent).
3. **Leader merge.** After `tuplesort_performsort`, the leader streams
   entries in (key, category) order, merging TID lists of equal keys
   (`GinBufferStoreTuple` logic — TID lists arrive sorted, merge is linear)
   and inserts each merged entry into the tree once. Insertion into a fresh
   index in fully sorted order is the cheapest possible build path.
4. **Fallback.** `rumbuild()` keeps the serial path when
   `parallel_workers == 0`, the table is too small, or the index has
   attribute types without sortsupport for the packed key comparison.

## RUM-specific deltas vs GIN

- **Posting payloads.** RUM stores an "addInfo" datum with each posting
  (this is what documentdb's extended op-classes use for composite/order
  data). The shared-tuplesort tuple format must carry (key, category, TID,
  addInfo) — TID list packing becomes (TID, addInfo) pair packing. This is
  the main divergence from PG18 GIN and the bulk of the port.
- **Pending list.** RUM has no fastupdate pending list to worry about at
  build time (build inserts directly) — simpler than GIN here.
- **WAL.** Neon requires full WAL for every page; parallel build does not
  change WAL volume, only wall-clock. Generic WAL records (RUM uses generic
  WAL when not core) — verify volume amplification is unchanged.

## Expected gains

Build is scan-bound + sort-bound; with N workers the scan and accumulate
phases parallelize ~linearly. On the measured 8.8M-doc collection with 2
workers on class l (2 vCPU reserved, burstable): expect ~2x on the scan
phase; combined with the non-concurrent fast path (already shipped:
`indexBuildsScheduledOnBgWorker` + `indexBuildNonConcurrentWhenIdle`),
520s → ~150s territory.

## Plan

- [ ] M1: `RumBuildShared` + parallel scan + per-worker accumulate into
      shared tuplesort, leader merge, **without addInfo** (reject parallel
      when any column uses addInfo) — validates the skeleton on plain
      B-tree-ish RUM columns.
- [ ] M2: (TID, addInfo) pair packing in the shared tuple format — covers
      documentdb composite op-classes (the real workload).
- [ ] M3: `documentdb.enableParallelIndexBuild` GUC (default off),
      `maintenance_work_mem` split, progress reporting
      (`pg_stat_progress_create_index` phases).
- [ ] M4: correctness harness — build serial vs parallel on the same data,
      compare `rumvalidate` + full index scans; perf run on the dev-saldanha
      dataset copy.
- [ ] Upstream PR.

## References

- PG18 `src/backend/access/gin/gininsert.c` (parallel build machinery,
  `_gin_parallel_build_main`, `GinBuffer`, `tuplesort_*_gintuple`).
- `pg_documentdb_extended_rum/src/ruminsert.c` (`rumbuild`,
  `rumBuildCallback`, `BuildAccumulator`).
- FeronDB measurements: docs em ferondb-cloud/docs/AUTO-INDEX-ADVISOR.md
  (pipeline async + fast path idle já em produção).
