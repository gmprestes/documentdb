SET search_path TO documentdb_api,documentdb_core,documentdb_api_catalog;

SET documentdb.next_collection_id TO 8300;
SET documentdb.next_collection_index_id TO 8300;

set documentdb.enableIndexDistinctScan to on;

-- base collection: 4 values, literal null, missing path
SELECT COUNT(documentdb_api.insert_one('rdedb', 'rdec',
    FORMAT('{ "_id": %s, "st": "S%s", "v": %s }', i, i % 4, i)::bson))
FROM generate_series(1, 40) i;
SELECT 1 FROM documentdb_api.insert_one('rdedb', 'rdec', '{ "_id": 100, "v": 100 }');
SELECT 1 FROM documentdb_api.insert_one('rdedb', 'rdec', '{ "_id": 101, "st": null, "v": 101 }');
SELECT documentdb_api_internal.create_indexes_non_concurrently('rdedb',
  '{ "createIndexes": "rdec", "indexes": [ { "key": { "st": 1 }, "enableCompositeTerm": true, "name": "st_1" } ] }', true);

-- the fast path takes over the whole plan
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_distinct('rdedb', '{ "distinct": "rdec", "key": "st" }');

-- values are exact and ordered; heap fallback agrees on the set
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdec", "key": "st" }');
set documentdb.enableIndexDistinctScan to off;
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdec", "key": "st" }');
set documentdb.enableIndexDistinctScan to on;

-- MVCC: a value deleted in this transaction disappears without vacuum
BEGIN;
SELECT 1 FROM documentdb_api.delete('rdedb', '{ "delete": "rdec", "deletes": [ { "q": { "st": "S3" }, "limit": 0 } ]}');
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdec", "key": "st" }');
ROLLBACK;

-- ... and comes back after rollback
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdec", "key": "st" }');

-- MVCC: dead entries left by a committed delete stay suppressed pre-vacuum
SELECT 1 FROM documentdb_api.delete('rdedb', '{ "delete": "rdec", "deletes": [ { "q": { "st": "S2" }, "limit": 0 } ]}');
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdec", "key": "st" }');
SELECT FORMAT('VACUUM documentdb_data.documents_%s', collection_id) FROM documentdb_api_catalog.collections WHERE collection_name = 'rdec' \gexec
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdec", "key": "st" }');

-- MVCC: own-transaction inserts are visible; rolled back inserts are not
BEGIN;
SELECT 1 FROM documentdb_api.insert_one('rdedb', 'rdec', '{ "_id": 200, "st": "SNEW" }');
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdec", "key": "st" }');
ROLLBACK;
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdec", "key": "st" }');

-- MVCC: an update moves the document between values in the same transaction
BEGIN;
SELECT 1 FROM documentdb_api.update('rdedb', '{ "update": "rdec", "updates": [ { "q": { "st": "S1" }, "u": { "$set": { "st": "S1B" } }, "multi": true } ]}');
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdec", "key": "st" }');
ROLLBACK;

-- a filter keeps the regular distinct plan
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_distinct('rdedb', '{ "distinct": "rdec", "key": "st", "query": { "v": { "$gt": 10 } } }');

-- a value with a posting tree (many documents) still enumerates once
SELECT COUNT(documentdb_api.insert_one('rdedb', 'rdebig',
    FORMAT('{ "_id": %s, "st": "BIG" }', i)::bson))
FROM generate_series(1, 3000) i;
SELECT 1 FROM documentdb_api.insert_one('rdedb', 'rdebig', '{ "_id": 100000, "st": "TINY" }');
SELECT documentdb_api_internal.create_indexes_non_concurrently('rdedb',
  '{ "createIndexes": "rdebig", "indexes": [ { "key": { "st": 1 }, "enableCompositeTerm": true, "name": "st_1" } ] }', true);
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdebig", "key": "st" }');

-- ... and dies correctly when every document of the big value is deleted
SELECT 1 FROM documentdb_api.delete('rdedb', '{ "delete": "rdebig", "deletes": [ { "q": { "st": "BIG" }, "limit": 0 } ]}');
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdebig", "key": "st" }');

-- arrays (multikey) fall back to the heap distinct with element semantics
SELECT 1 FROM documentdb_api.insert_one('rdedb', 'rdemk', '{ "_id": 1, "st": ["a", "b"] }');
SELECT 1 FROM documentdb_api.insert_one('rdedb', 'rdemk', '{ "_id": 2, "st": "c" }');
SELECT documentdb_api_internal.create_indexes_non_concurrently('rdedb',
  '{ "createIndexes": "rdemk", "indexes": [ { "key": { "st": 1 }, "enableCompositeTerm": true, "name": "st_1" } ] }', true);
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdemk", "key": "st" }');

-- truncated terms fall back
SELECT 1 FROM documentdb_api.insert_one('rdedb', 'rdetrunc', FORMAT('{ "_id": 1, "st": "%s" }', repeat('y', 4000))::bson);
SELECT 1 FROM documentdb_api.insert_one('rdedb', 'rdetrunc', '{ "_id": 2, "st": "small" }');
SELECT documentdb_api_internal.create_indexes_non_concurrently('rdedb',
  '{ "createIndexes": "rdetrunc", "indexes": [ { "key": { "st": 1 }, "enableCompositeTerm": true, "name": "st_1" } ] }', true);
SELECT LENGTH(documentdb_core.bson_to_json_string(documentdb_api.distinct_query('rdedb', '{ "distinct": "rdetrunc", "key": "st" }'))::text) > 100;

-- partial filter expression indexes fall back
SELECT COUNT(documentdb_api.insert_one('rdedb', 'rdepfe',
    FORMAT('{ "_id": %s, "st": "S%s", "v": %s }', i, i % 2, i)::bson))
FROM generate_series(1, 10) i;
SELECT documentdb_api_internal.create_indexes_non_concurrently('rdedb',
  '{ "createIndexes": "rdepfe", "indexes": [ { "key": { "st": 1 }, "enableCompositeTerm": true, "name": "st_pfe", "partialFilterExpression": { "v": { "$gt": 5 } } } ] }', true);
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdepfe", "key": "st" }');

-- descending index still enumerates (reverse key order is fine for distinct)
SELECT COUNT(documentdb_api.insert_one('rdedb', 'rdedesc',
    FORMAT('{ "_id": %s, "st": "S%s" }', i, i % 3)::bson))
FROM generate_series(1, 12) i;
SELECT documentdb_api_internal.create_indexes_non_concurrently('rdedb',
  '{ "createIndexes": "rdedesc", "indexes": [ { "key": { "st": -1 }, "enableCompositeTerm": true, "name": "st_m1" } ] }', true);
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdedesc", "key": "st" }');

-- empty collection
SELECT documentdb_api.create_collection('rdedb', 'rdeempty');
SELECT documentdb_api_internal.create_indexes_non_concurrently('rdedb',
  '{ "createIndexes": "rdeempty", "indexes": [ { "key": { "st": 1 }, "enableCompositeTerm": true, "name": "st_1" } ] }', true);
SELECT documentdb_api.distinct_query('rdedb', '{ "distinct": "rdeempty", "key": "st" }');

-- collection without any candidate index keeps the regular plan
SELECT 1 FROM documentdb_api.insert_one('rdedb', 'rdenoidx', '{ "_id": 1, "st": "x" }');
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_distinct('rdedb', '{ "distinct": "rdenoidx", "key": "st" }');
