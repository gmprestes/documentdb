CREATE OR REPLACE FUNCTION __API_SCHEMA_INTERNAL_V2__.bson_distinct_index_scan(text, text, text)
 RETURNS __CORE_SCHEMA__.bson
 LANGUAGE c
 STABLE PARALLEL RESTRICTED STRICT
AS 'MODULE_PATHNAME', $function$bson_distinct_index_scan$function$;
