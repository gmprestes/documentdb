SET search_path TO documentdb_api,documentdb_core,documentdb_api_catalog;

SET documentdb.next_collection_id TO 7900;
SET documentdb.next_collection_index_id TO 7900;

set documentdb.enableIndexOrderbyPushdown to on;
set documentdb.enableIndexOnlyScan to on;
set seq_page_cost to 1000;

-- 20 docs across 4 values, plus literal null and a missing path
SELECT COUNT(documentdb_api.insert_one('diosdb', 'diosc',
    FORMAT('{ "_id": %s, "st": "S%s", "v": %s }', i, i % 4, i)::bson))
FROM generate_series(1, 20) i;
SELECT 1 FROM documentdb_api.insert_one('diosdb', 'diosc', '{ "_id": 100, "v": 100 }');
SELECT 1 FROM documentdb_api.insert_one('diosdb', 'diosc', '{ "_id": 101, "st": null, "v": 101 }');

SELECT documentdb_api_internal.create_indexes_non_concurrently('diosdb',
  '{ "createIndexes": "diosc", "indexes": [ { "key": { "st": 1 }, "enableCompositeTerm": true, "name": "st_1" } ] }', true);

SELECT FORMAT('VACUUM (ANALYZE ON, FREEZE ON) documentdb_data.documents_%s', collection_id) FROM documentdb_api_catalog.collections WHERE collection_name = 'diosc' \gexec

-- distinct is served by a covered index only scan
EXPLAIN (ANALYZE ON, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_distinct('diosdb', '{ "distinct": "diosc", "key": "st" }');

-- results: literal null included, missing path excluded; identical to the heap fallback
SELECT documentdb_api.distinct_query('diosdb', '{ "distinct": "diosc", "key": "st" }');
set documentdb.enableIndexOnlyScan to off;
SELECT documentdb_api.distinct_query('diosdb', '{ "distinct": "diosc", "key": "st" }');
set documentdb.enableIndexOnlyScan to on;

-- group by the indexed key with a count: covered ordered scan, no heap access
EXPLAIN (ANALYZE ON, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_pipeline('diosdb', '{ "aggregate": "diosc", "pipeline": [ { "$group": { "_id": "$st", "c": { "$sum": 1 } } } ], "cursor": {} }');
SELECT document FROM bson_aggregation_pipeline('diosdb', '{ "aggregate": "diosc", "pipeline": [ { "$group": { "_id": "$st", "c": { "$sum": 1 } } }, { "$sort": { "_id": 1 } } ], "cursor": {} }');
set documentdb.enableIndexOnlyScan to off;
SELECT document FROM bson_aggregation_pipeline('diosdb', '{ "aggregate": "diosc", "pipeline": [ { "$group": { "_id": "$st", "c": { "$sum": 1 } } }, { "$sort": { "_id": 1 } } ], "cursor": {} }');
set documentdb.enableIndexOnlyScan to on;

-- $sum over a second covered column
SELECT documentdb_api_internal.create_indexes_non_concurrently('diosdb',
  '{ "createIndexes": "diosc", "indexes": [ { "key": { "st": 1, "v": 1 }, "enableCompositeTerm": true, "name": "st_v" } ] }', true);
EXPLAIN (ANALYZE ON, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_pipeline('diosdb', '{ "aggregate": "diosc", "pipeline": [ { "$group": { "_id": "$st", "t": { "$sum": "$v" } } } ], "cursor": {} }');
SELECT document FROM bson_aggregation_pipeline('diosdb', '{ "aggregate": "diosc", "pipeline": [ { "$group": { "_id": "$st", "t": { "$sum": "$v" } } }, { "$sort": { "_id": 1 } } ], "cursor": {} }');

-- a non-first column of a composite index is also covered
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_distinct('diosdb', '{ "distinct": "diosc", "key": "v" }');
SELECT documentdb_api.distinct_query('diosdb', '{ "distinct": "diosc", "key": "v" }');
set documentdb.enableIndexOnlyScan to off;
SELECT documentdb_api.distinct_query('diosdb', '{ "distinct": "diosc", "key": "v" }');
set documentdb.enableIndexOnlyScan to on;

-- dotted paths are not covered (index term reconstruction writes a literal top-level key)
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_distinct('diosdb', '{ "distinct": "diosc", "key": "st.x" }');

-- an empty array makes the index multikey: exact fallback, [] is a distinct group key
SELECT COUNT(documentdb_api.insert_one('diosdb', 'diosarr',
    FORMAT('{ "_id": %s, "st": "S%s" }', i, i % 2)::bson))
FROM generate_series(1, 10) i;
SELECT 1 FROM documentdb_api.insert_one('diosdb', 'diosarr', '{ "_id": 100, "st": [] }');
SELECT 1 FROM documentdb_api.insert_one('diosdb', 'diosarr', '{ "_id": 101 }');
SELECT documentdb_api_internal.create_indexes_non_concurrently('diosdb',
  '{ "createIndexes": "diosarr", "indexes": [ { "key": { "st": 1 }, "enableCompositeTerm": true, "name": "st_1" } ] }', true);
SELECT FORMAT('VACUUM (ANALYZE ON, FREEZE ON) documentdb_data.documents_%s', collection_id) FROM documentdb_api_catalog.collections WHERE collection_name = 'diosarr' \gexec
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_distinct('diosdb', '{ "distinct": "diosarr", "key": "st" }');
SELECT documentdb_api.distinct_query('diosdb', '{ "distinct": "diosarr", "key": "st" }');
SELECT document FROM bson_aggregation_pipeline('diosdb', '{ "aggregate": "diosarr", "pipeline": [ { "$group": { "_id": "$st", "c": { "$sum": 1 } } }, { "$sort": { "_id": 1 } } ], "cursor": {} }');

-- non-empty arrays (multikey) also fall back
SELECT 1 FROM documentdb_api.insert_one('diosdb', 'diosmk', '{ "_id": 1, "st": ["a", "b"] }');
SELECT 1 FROM documentdb_api.insert_one('diosdb', 'diosmk', '{ "_id": 2, "st": "c" }');
SELECT documentdb_api_internal.create_indexes_non_concurrently('diosdb',
  '{ "createIndexes": "diosmk", "indexes": [ { "key": { "st": 1 }, "enableCompositeTerm": true, "name": "st_1" } ] }', true);
SELECT FORMAT('VACUUM (ANALYZE ON, FREEZE ON) documentdb_data.documents_%s', collection_id) FROM documentdb_api_catalog.collections WHERE collection_name = 'diosmk' \gexec
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_distinct('diosdb', '{ "distinct": "diosmk", "key": "st" }');
SELECT documentdb_api.distinct_query('diosdb', '{ "distinct": "diosmk", "key": "st" }');

-- a partial filter expression index cannot serve covered projections
SELECT COUNT(documentdb_api.insert_one('diosdb', 'diospfe',
    FORMAT('{ "_id": %s, "st": "S%s", "v": %s }', i, i % 2, i)::bson))
FROM generate_series(1, 10) i;
SELECT documentdb_api_internal.create_indexes_non_concurrently('diosdb',
  '{ "createIndexes": "diospfe", "indexes": [ { "key": { "st": 1 }, "enableCompositeTerm": true, "name": "st_pfe", "partialFilterExpression": { "v": { "$gt": 5 } } } ] }', true);
SELECT FORMAT('VACUUM (ANALYZE ON, FREEZE ON) documentdb_data.documents_%s', collection_id) FROM documentdb_api_catalog.collections WHERE collection_name = 'diospfe' \gexec
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_distinct('diosdb', '{ "distinct": "diospfe", "key": "st" }');
SELECT documentdb_api.distinct_query('diosdb', '{ "distinct": "diospfe", "key": "st" }');

-- truncated terms disable the covered scan for the whole index
SELECT 1 FROM documentdb_api.insert_one('diosdb', 'diostrunc', FORMAT('{ "_id": 1, "st": "%s" }', repeat('x', 4000))::bson);
SELECT 1 FROM documentdb_api.insert_one('diosdb', 'diostrunc', '{ "_id": 2, "st": "small" }');
SELECT documentdb_api_internal.create_indexes_non_concurrently('diosdb',
  '{ "createIndexes": "diostrunc", "indexes": [ { "key": { "st": 1 }, "enableCompositeTerm": true, "name": "st_1" } ] }', true);
SELECT FORMAT('VACUUM (ANALYZE ON, FREEZE ON) documentdb_data.documents_%s', collection_id) FROM documentdb_api_catalog.collections WHERE collection_name = 'diostrunc' \gexec
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_distinct('diosdb', '{ "distinct": "diostrunc", "key": "st" }');

-- descending single-key composite index also serves the covered scan
SELECT COUNT(documentdb_api.insert_one('diosdb', 'diosdesc',
    FORMAT('{ "_id": %s, "st": "S%s" }', i, i % 3)::bson))
FROM generate_series(1, 12) i;
SELECT documentdb_api_internal.create_indexes_non_concurrently('diosdb',
  '{ "createIndexes": "diosdesc", "indexes": [ { "key": { "st": -1 }, "enableCompositeTerm": true, "name": "st_m1" } ] }', true);
SELECT FORMAT('VACUUM (ANALYZE ON, FREEZE ON) documentdb_data.documents_%s', collection_id) FROM documentdb_api_catalog.collections WHERE collection_name = 'diosdesc' \gexec
EXPLAIN (ANALYZE ON, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_distinct('diosdb', '{ "distinct": "diosdesc", "key": "st" }');
SELECT documentdb_api.distinct_query('diosdb', '{ "distinct": "diosdesc", "key": "st" }');
set documentdb.enableIndexOnlyScan to off;
SELECT documentdb_api.distinct_query('diosdb', '{ "distinct": "diosdesc", "key": "st" }');
set documentdb.enableIndexOnlyScan to on;

-- distinct with an indexable filter on the same key still covers via the filter clause
EXPLAIN (ANALYZE ON, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT document FROM bson_aggregation_distinct('diosdb', '{ "distinct": "diosc", "key": "st", "query": { "st": { "$gt": "S1" } } }');
SELECT documentdb_api.distinct_query('diosdb', '{ "distinct": "diosc", "key": "st", "query": { "st": { "$gt": "S1" } } }');
set documentdb.enableIndexOnlyScan to off;
SELECT documentdb_api.distinct_query('diosdb', '{ "distinct": "diosc", "key": "st", "query": { "st": { "$gt": "S1" } } }');
set documentdb.enableIndexOnlyScan to on;
