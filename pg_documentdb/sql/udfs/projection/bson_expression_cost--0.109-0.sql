/*
 * Field extraction walks and detoasts the whole document per call
 * (~15us on 1KB documents); the default COST 1 makes the planner treat it
 * as free and underinvest in parallel plans. Price it realistically.
 */
ALTER FUNCTION __API_CATALOG_SCHEMA__.bson_expression_get(__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson, bool) COST 50;
ALTER FUNCTION __API_SCHEMA_INTERNAL_V2__.bson_expression_get(__CORE_SCHEMA_V2__.bson, __CORE_SCHEMA_V2__.bson, bool, __CORE_SCHEMA_V2__.bson) COST 50;
ALTER FUNCTION __API_SCHEMA_INTERNAL_V2__.bson_expression_get(__CORE_SCHEMA_V2__.bson, __CORE_SCHEMA_V2__.bson, bool, __CORE_SCHEMA_V2__.bson, text) COST 50;
ALTER FUNCTION __API_SCHEMA_INTERNAL_V2__.bson_expression_partition_by_fields_get(__CORE_SCHEMA__.bson, __CORE_SCHEMA__.bson) COST 50;
