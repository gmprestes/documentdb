SET search_path TO documentdb_api,documentdb_core,documentdb_api_catalog;

SET documentdb.next_collection_id TO 8600;
SET documentdb.next_collection_index_id TO 8600;

-- deterministic parallel plans
set parallel_setup_cost to 0;
set parallel_tuple_cost to 0;
set min_parallel_table_scan_size to 0;
set max_parallel_workers_per_gather to 2;

SELECT COUNT(documentdb_api.insert_one('perfdb', 'perfc',
    FORMAT('{ "_id": %s, "st": "S%s", "v": %s, "d": { "$date": { "$numberLong": "%s" } } }',
           i, i % 4, i, 1700000000000::int8 + (i::int8 * 86400000))::bson))
FROM generate_series(1, 100) i;
SELECT FORMAT('VACUUM (ANALYZE) documentdb_data.documents_%s', collection_id) FROM documentdb_api_catalog.collections WHERE collection_name = 'perfc' \gexec

-- aggregate support functions are parallel safe: $group + $sum plans a
-- Finalize/Partial pair under the Gather
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_pipeline('perfdb', '{ "aggregate": "perfc", "pipeline": [ { "$group": { "_id": "$st", "q": { "$sum": "$v" } } } ], "cursor": {} }');
SELECT document FROM bson_aggregation_pipeline('perfdb', '{ "aggregate": "perfc", "pipeline": [ { "$group": { "_id": "$st", "q": { "$sum": "$v" } } }, { "$sort": { "_id": 1 } } ], "cursor": {} }');

-- date parts come from the local decomposition; results across operators
SELECT document FROM bson_aggregation_pipeline('perfdb', '{ "aggregate": "perfc", "pipeline": [ { "$match": { "_id": { "$in": [1, 50, 100] } } }, { "$project": { "s": { "$dateToString": { "date": "$d", "format": "%Y-%m-%dT%H:%M:%S.%L j%j w%w U%U V%V G%G u%u", "timezone": "America/Sao_Paulo" } }, "parts": { "$dateToParts": { "date": "$d", "timezone": "Asia/Katmandu" } }, "wk": { "$week": "$d" }, "iso": { "$isoWeekYear": "$d" } } }, { "$sort": { "_id": 1 } } ], "cursor": {} }');

-- default format + utc offset timezone
SELECT document FROM bson_aggregation_pipeline('perfdb', '{ "aggregate": "perfc", "pipeline": [ { "$match": { "_id": 1 } }, { "$project": { "s": { "$dateToString": { "date": "$d" } }, "so": { "$dateToString": { "date": "$d", "timezone": "+05:45" } } } } ], "cursor": {} }');

-- facet: inline base gated behind the GUC; results identical either way
set documentdb.enableFacetInlineBase to on;
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_pipeline('perfdb', '{ "aggregate": "perfc", "pipeline": [ { "$match": { "st": { "$in": ["S1", "S2"] } } }, { "$facet": { "a": [ { "$group": { "_id": "$st", "n": { "$sum": 1 } } }, { "$sort": { "_id": 1 } } ], "b": [ { "$count": "c" } ] } } ], "cursor": {} }');
SELECT document FROM bson_aggregation_pipeline('perfdb', '{ "aggregate": "perfc", "pipeline": [ { "$match": { "st": { "$in": ["S1", "S2"] } } }, { "$facet": { "a": [ { "$group": { "_id": "$st", "n": { "$sum": 1 } } }, { "$sort": { "_id": 1 } } ], "b": [ { "$count": "c" } ] } } ], "cursor": {} }');
set documentdb.enableFacetInlineBase to off;
SELECT document FROM bson_aggregation_pipeline('perfdb', '{ "aggregate": "perfc", "pipeline": [ { "$match": { "st": { "$in": ["S1", "S2"] } } }, { "$facet": { "a": [ { "$group": { "_id": "$st", "n": { "$sum": 1 } } }, { "$sort": { "_id": 1 } } ], "b": [ { "$count": "c" } ] } } ], "cursor": {} }');
set documentdb.enableFacetInlineBase to on;

-- $limit before $facet is not re-execution safe: base stays materialized
EXPLAIN (COSTS OFF) SELECT document FROM bson_aggregation_pipeline('perfdb', '{ "aggregate": "perfc", "pipeline": [ { "$limit": 10 }, { "$facet": { "a": [ { "$group": { "_id": "$st", "n": { "$sum": 1 } } } ], "b": [ { "$count": "c" } ] } } ], "cursor": {} }');

-- single-branch facet keeps its existing plan shape
SELECT document FROM bson_aggregation_pipeline('perfdb', '{ "aggregate": "perfc", "pipeline": [ { "$match": { "st": "S1" } }, { "$facet": { "only": [ { "$count": "c" } ] } } ], "cursor": {} }');
