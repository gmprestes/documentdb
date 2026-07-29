# Parallel index builds for pg_documentdb_extended_rum

Status: DESIGN (revised 29/jul/2026 after reading the shipped code).
Branch: `feron-parallel-rum`.

## The situation, corrected

An earlier version of this document proposed porting PostgreSQL 18's parallel
GIN build to RUM. **That work is unnecessary: `pg_documentdb_extended_rum`
already ships a complete parallel build path** — `_rum_begin_parallel`,
`_rum_parallel_heapscan`, `_rum_parallel_merge`, a shared tuplesort
(`rumbuild_tuplesort.c`) and the worker entry point
`documentdb_rum_parallel_build_main`. The GUC
`documentdb_rum.enable_parallel_index_build` defaults to **on**.

The problem is that **DocumentDB's own indexes never reach that path.**

## Measurement

On a stock compute image (PG17, `max_parallel_maintenance_workers = 2`,
`maintenance_work_mem` raised to 1GB, `parallel_index_workers_override = 2`),
creating an ordinary single-field index through `createIndexes`:

```
DEBUG:  CREATE INDEX documents_rum_index_4 ON documentdb_data.documents_2
        USING documentdb_rum (document bson_rum_composite_path_ops(pathspec='["s"]', tl=2691))
DEBUG:  building index "documents_rum_index_4" on table "documents_2" serially
```

Serial, with parallelism explicitly requested. The reason is in `rumbuild()`:

```c
/* Scenarios that have addinfo need to skip parallel build */
for (i = 0; i < INDEX_MAX_KEYS && isSortedIndexBuildCapable; i++)
{
    if (buildstate.rumstate.addAttrs[i] != NULL)      { isSortedIndexBuildCapable = false; break; }
    if (buildstate.rumstate.canJoinAddInfo[i])        { isSortedIndexBuildCapable = false; break; }
}
if (buildstate.rumstate.attrnAddToColumn != InvalidAttrNumber)
    isSortedIndexBuildCapable = false;
```

Every DocumentDB opclass (`bson_rum_composite_path_ops`,
`bson_rum_single_path_ops`, text ops) carries **addInfo** — that is where the
term metadata lives. So the exclusion is not an edge case: it covers
essentially 100% of user index builds, which is why an 8.8M-document
collection took ~520s single-core in production.

A second, smaller gate: `plan_create_index_workers` requires 32MB of
`maintenance_work_mem` per worker, so small classes get zero workers anyway.

## What actually needs to be built

Teach the shared tuplesort to carry addInfo, then lift the exclusion.

- **Tuple format.** The shared sort currently packs `(key, category, TID
  list)`. It needs `(key, category, [(TID, addInfo)])` — addInfo is a Datum
  whose type comes from the opclass (`addInfoTypeOid`), so packing must
  handle by-value and varlena forms, plus a null bitmap (addInfo is
  optional per posting).
- **Merge side.** `_rum_parallel_merge` merges TID lists of equal keys; it
  must merge the paired arrays instead, preserving TID order (the existing
  invariant) and keeping each addInfo with its TID.
- **canJoinAddInfo.** Opclasses that can *combine* addInfo across postings
  (`canJoinAddInfo[i]`) need their join function applied during the merge,
  not just concatenation. If that turns out to be expensive to do correctly,
  M1 can keep excluding those specific attributes and cover the rest.
- **Eligibility.** Replace the blanket exclusion with a per-attribute check:
  parallel is allowed when every attribute either has no addInfo or has an
  addInfo type the packer supports.
- **Memory gate.** Consider lowering the per-worker requirement for RUM (it
  accumulates differently from btree) or documenting that classes below
  ~2GB `maintenance_work_mem` stay serial by design.

## Milestones

- [ ] **M1** — addInfo packing in the shared tuple + merge that preserves
      pairs; eligibility opened for attributes with plain (non-joinable)
      addInfo. Correctness harness: build the same collection serially and
      in parallel, compare `pg_relation_size`, full index scans and query
      results.
- [ ] **M2** — `canJoinAddInfo` attributes (apply the join during merge).
- [ ] **M3** — memory/worker heuristics tuned for RUM; progress reporting
      through `pg_stat_progress_create_index` in the parallel path.
- [ ] **M4** — benchmark on a production-sized collection (8.8M docs), then
      upstream PR.

## Expected gain

The build is scan- and sort-bound. With 2 workers on the classes we run
(2 vCPU reserved, burstable), the earlier 520s serial build should land
near 250-300s; combined with the non-concurrent fast path already shipped
(`indexBuildNonConcurrentWhenIdle`), well under 200s.

## Why this is worth doing upstream

The parallel machinery is already written and tested — the only thing
standing between it and every DocumentDB user is addInfo support in one
tuple format. That is a contained, high-leverage change.
