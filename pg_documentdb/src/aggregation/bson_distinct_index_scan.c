/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/aggregation/bson_distinct_index_scan.c
 *
 * Answers the distinct command by enumerating the entry tree of a composite
 * RUM index: one visibility-checked callback per distinct key instead of one
 * tuple per document, making distinct O(distinct values) for covered keys.
 *
 * The command layer routes here (see TryGenerateIndexDistinctQuery) only for
 * filter-less distinct over an existing collection when the feature flag is
 * on and a candidate index exists. Every correctness gate is (re)checked at
 * execution time against the open index; anything ineligible - no composite
 * index on the key, multikey (arrays), truncated terms, partial filter,
 * wildcard - falls back to the exact heap-based distinct via SPI, using the
 * same SQL shape the gateway issues.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <fmgr.h>
#include <miscadmin.h>
#include <access/genam.h>
#include <access/table.h>
#include <catalog/namespace.h>
#include <executor/spi.h>
#include <parser/parse_node.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/snapmgr.h>

#include "io/bson_core.h"
#include "metadata/metadata_cache.h"
#include "aggregation/bson_aggregation_pipeline.h"
#include "aggregation/bson_aggregation_pipeline_private.h"
#include "metadata/collection.h"
#include "metadata/index.h"
#include "metadata/metadata_cache.h"
#include "index_am/index_am_exports.h"
#include "index_am/index_am_utils.h"
#include "index_am/documentdb_rum.h"
#include "opclass/bson_gin_index_mgmt.h"
#include "opclass/bson_gin_index_term.h"
#include "query/bson_compare.h"
#include "utils/documentdb_errors.h"
#include "utils/query_utils.h"

extern bool EnableIndexDistinctScan;

PG_FUNCTION_INFO_V1(bson_distinct_index_scan);

/* Accumulates distinct values during one entry tree enumeration. */
typedef struct DistinctIndexScanState
{
	/* composite column of the index that holds the distinct key */
	int32_t columnNumber;

	/* a truncated term was found: results cannot be reconstructed */
	bool sawTruncatedTerm;

	/* previous emitted term (palloc'd copy) for adjacent deduplication */
	bytea *lastTermCopy;

	pgbson_writer responseWriter;
	pgbson_array_writer valuesWriter;
} DistinctIndexScanState;


/*
 * Entry tree callback: decodes the composite term, skips non-values
 * (missing paths, empty arrays) and appends new values to the response.
 * Mongo equality (cross numeric types) can merge adjacent index terms of
 * different types, so dedup compares against the previously emitted term.
 */
static bool
DistinctIndexScanEntryCallback(Datum entryKey, int category, void *context)
{
	DistinctIndexScanState *state = (DistinctIndexScanState *) context;
	bytea *termBytes = DatumGetByteaPP(entryKey);

	if (IsSerializedIndexTermTruncated(termBytes))
	{
		state->sawTruncatedTerm = true;
		return false;
	}

	BsonIndexTerm terms[INDEX_MAX_KEYS] = { 0 };
	int32_t numTerms = InitializeCompositeIndexTerm(termBytes, terms);
	if (state->columnNumber >= numTerms)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg(
							"Index term has %d columns but distinct key expected column %d",
							numTerms, state->columnNumber)));
	}

	BsonIndexTerm *term = &terms[state->columnNumber];

	if (IsIndexTermValueUndefined(term) || IsIndexTermMaybeUndefined(term))
	{
		/* Path missing in the document: contributes nothing to distinct */
		return true;
	}

	if (term->element.bsonValue.value_type == BSON_TYPE_UNDEFINED)
	{
		/* Literal-undefined value: an empty array, which unwinds to nothing.
		 * (Unreachable behind the multikey gate; defense in depth.)
		 */
		return true;
	}

	if (state->lastTermCopy != NULL)
	{
		BsonIndexTerm lastTerms[INDEX_MAX_KEYS] = { 0 };
		InitializeCompositeIndexTerm(state->lastTermCopy, lastTerms);
		if (BsonValueEquals(&lastTerms[state->columnNumber].element.bsonValue,
							&term->element.bsonValue))
		{
			return true;
		}

		pfree(state->lastTermCopy);
		state->lastTermCopy = NULL;
	}

	PgbsonArrayWriterWriteValue(&state->valuesWriter, &term->element.bsonValue);

	/* entryKey points into a transient page copy: keep our own copy */
	Size termSize = VARSIZE_ANY(termBytes);
	state->lastTermCopy = (bytea *) palloc(termSize);
	memcpy(state->lastTermCopy, termBytes, termSize);

	return true;
}


/*
 * Opens and returns the first index of the collection that can enumerate the
 * distinct key exactly: a valid, non-partial, non-wildcard, non-multikey,
 * non-truncated composite index whose first column is the key. Returns NULL
 * with *columnNumber untouched when there is none.
 */
static Relation
OpenEligibleDistinctIndex(MongoCollection *collection, const char *distinctKey,
						  int32_t *columnNumber)
{
	bool excludeIdIndex = true;
	bool enableNestedDistribution = false;
	List *indexes = CollectionIdGetValidIndexes(collection->collectionId,
												excludeIdIndex,
												enableNestedDistribution);

	Oid dataNamespaceOid = get_namespace_oid(ApiDataSchemaName, false);

	ListCell *cell;
	foreach(cell, indexes)
	{
		IndexDetails *details = (IndexDetails *) lfirst(cell);
		if (details->isIndexBuildInProgress)
		{
			continue;
		}

		char indexRelName[NAMEDATALEN];
		snprintf(indexRelName, NAMEDATALEN, DOCUMENT_DATA_TABLE_INDEX_NAME_FORMAT,
				 details->indexId);
		Oid indexRelationId = get_relname_relid(indexRelName, dataNamespaceOid);
		if (!OidIsValid(indexRelationId))
		{
			continue;
		}

		Relation indexRel = index_open(indexRelationId, AccessShareLock);

		GetMultikeyStatusFunc getMultiKeyStatusFunc = NULL;
		GetTruncationStatusFunc getTruncationStatusFunc = NULL;
		if (!GetIndexAmSupportsIndexOnlyScan(indexRel->rd_rel->relam,
											 indexRel->rd_opfamily[0],
											 &getMultiKeyStatusFunc,
											 &getTruncationStatusFunc) ||
			getMultiKeyStatusFunc == NULL || getTruncationStatusFunc == NULL ||
			GetEnumerateVisibleEntriesFuncByRelAm(indexRel->rd_rel->relam) == NULL)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		if (RelationGetIndexPredicate(indexRel) != NIL)
		{
			/* Partial filter / sparse indexes do not see all documents */
			index_close(indexRel, AccessShareLock);
			continue;
		}

		bytea **attOptions = RelationGetIndexAttOptions(indexRel, true);
		if (attOptions == NULL || attOptions[0] == NULL)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		BsonGinIndexOptionsBase *options =
			(BsonGinIndexOptionsBase *) attOptions[0];
		if (options->type != IndexOptionsType_Composite)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		BsonGinCompositePathOptions *compositeOptions =
			(BsonGinCompositePathOptions *) options;
		if (compositeOptions->wildcardPathIndex >= 0)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		/* Adjacent dedup in the callback relies on the enumeration being
		 * ordered by the distinct key, so only the first column qualifies.
		 */
		int8_t sortDirection = 0;
		int32_t column = GetCompositeOpClassColumnNumber(distinctKey,
														 (void *) options,
														 &sortDirection);
		if (column != 0)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		if (getMultiKeyStatusFunc(indexRel) || getTruncationStatusFunc(indexRel))
		{
			/* Arrays make entry counts diverge from documents and truncated
			 * terms cannot be reconstructed - both need the heap fallback.
			 */
			index_close(indexRel, AccessShareLock);
			continue;
		}

		*columnNumber = column;
		return indexRel;
	}

	return NULL;
}


/*
 * Exact heap-based fallback: the same SQL the gateway uses for distinct.
 */
static Datum
RunDistinctHeapFallback(text *databaseText, text *collectionText, text *keyText)
{
	StringInfoData query;
	initStringInfo(&query);
	appendStringInfo(&query,
					 "WITH r1 AS (SELECT DISTINCT %s.bson_distinct_unwind(document, $3) AS document"
					 " FROM %s.collection($1, $2))"
					 " SELECT %s.bson_build_distinct_response(COALESCE(array_agg(document), '{}'::%s.bson[])) FROM r1",
					 ApiCatalogSchemaName, ApiSchemaName, ApiCatalogSchemaName,
					 CoreSchemaName);

	Oid argTypes[3] = { TEXTOID, TEXTOID, TEXTOID };
	Datum argValues[3] = {
		PointerGetDatum(databaseText), PointerGetDatum(collectionText),
		PointerGetDatum(keyText)
	};
	char *argNulls = NULL;
	bool readOnly = true;
	bool isNull = false;

	Datum result = ExtensionExecuteQueryWithArgsViaSPI(query.data, 3, argTypes,
													   argValues, argNulls,
													   readOnly, SPI_OK_SELECT,
													   &isNull);
	pfree(query.data);

	if (isNull)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg("Distinct fallback query returned null")));
	}

	return result;
}


/*
 * documentdb_api_internal.bson_distinct_index_scan(database, collection, key)
 *
 * Executes the distinct command via entry tree enumeration when an eligible
 * index exists, and via the exact heap plan otherwise.
 */
Datum
bson_distinct_index_scan(PG_FUNCTION_ARGS)
{
	text *databaseText = PG_GETARG_TEXT_P(0);
	text *collectionText = PG_GETARG_TEXT_P(1);
	text *keyText = PG_GETARG_TEXT_P(2);

	MongoCollection *collection = GetMongoCollectionByNameDatum(
		PointerGetDatum(databaseText), PointerGetDatum(collectionText),
		AccessShareLock);

	if (collection == NULL)
	{
		/* A collection that vanished (or is a view): the fallback handles it. */
		return RunDistinctHeapFallback(databaseText, collectionText, keyText);
	}

	int32_t columnNumber = 0;
	Relation indexRel = OpenEligibleDistinctIndex(collection,
												  text_to_cstring(keyText),
												  &columnNumber);
	if (indexRel == NULL)
	{
		return RunDistinctHeapFallback(databaseText, collectionText, keyText);
	}

	RumEnumerateVisibleEntriesFunc enumerateFunc =
		GetEnumerateVisibleEntriesFuncByRelAm(indexRel->rd_rel->relam);

	Relation heapRel = table_open(collection->relationId, AccessShareLock);

	DistinctIndexScanState state = { 0 };
	state.columnNumber = columnNumber;
	PgbsonWriterInit(&state.responseWriter);
	PgbsonWriterStartArray(&state.responseWriter, "values", 6,
						   &state.valuesWriter);

	/* The single bson attribute of the RUM index */
	OffsetNumber indexAttnum = 1;
	enumerateFunc(indexRel, heapRel, GetActiveSnapshot(), indexAttnum,
				  DistinctIndexScanEntryCallback, &state);

	table_close(heapRel, AccessShareLock);
	index_close(indexRel, AccessShareLock);

	if (state.sawTruncatedTerm)
	{
		/* Raced with a concurrent truncation-inducing insert of this same
		 * transaction; the values collected so far cannot be trusted.
		 */
		return RunDistinctHeapFallback(databaseText, collectionText, keyText);
	}

	PgbsonWriterEndArray(&state.responseWriter, &state.valuesWriter);
	PgbsonWriterAppendDouble(&state.responseWriter, "ok", 2, 1.0);

	PG_RETURN_POINTER(PgbsonWriterGetPgbson(&state.responseWriter));
}


/*
 * Build-time hook for GenerateDistinctQuery: when the fast path applies,
 * returns a replacement query that calls bson_distinct_index_scan; NULL
 * keeps the regular unwind-based distinct query.
 *
 * Only shape checks happen here (feature flag, no filter, a valid index
 * whose first key path is the distinct key); all correctness gates rerun at
 * execution against the open index, falling back via SPI as needed.
 */
Query *
TryGenerateIndexDistinctQuery(text *databaseDatum, const StringView *distinctKey,
							  const StringView *collectionName,
							  MongoCollection *collection)
{
	if (!EnableIndexDistinctScan || collection == NULL ||
		collection->viewDefinition != NULL)
	{
		return NULL;
	}

	/* Cheap candidate check: some valid index has the key as its first path */
	bool excludeIdIndex = true;
	bool enableNestedDistribution = false;
	List *indexes = CollectionIdGetValidIndexes(collection->collectionId,
												excludeIdIndex,
												enableNestedDistribution);
	bool hasCandidate = false;
	ListCell *cell;
	foreach(cell, indexes)
	{
		IndexDetails *details = (IndexDetails *) lfirst(cell);
		if (details->isIndexBuildInProgress ||
			details->indexSpec.indexKeyDocument == NULL)
		{
			continue;
		}

		pgbsonelement keyElement = { 0 };
		bson_iter_t keyIter;
		PgbsonInitIterator(details->indexSpec.indexKeyDocument, &keyIter);
		if (!bson_iter_next(&keyIter))
		{
			continue;
		}

		keyElement.path = bson_iter_key(&keyIter);
		keyElement.pathLength = bson_iter_key_len(&keyIter);
		keyElement.bsonValue = *bson_iter_value(&keyIter);

		if (keyElement.pathLength == distinctKey->length &&
			strncmp(keyElement.path, distinctKey->string,
					distinctKey->length) == 0 &&
			BsonValueIsNumber(&keyElement.bsonValue))
		{
			hasCandidate = true;
			break;
		}
	}

	if (!hasCandidate)
	{
		return NULL;
	}

	/* SELECT documentdb_api_internal.bson_distinct_index_scan(db, coll, key) */
	Query *query = makeNode(Query);
	query->commandType = CMD_SELECT;
	query->querySource = QSRC_ORIGINAL;
	query->canSetTag = true;
	query->jointree = makeNode(FromExpr);

	Const *databaseConst = makeConst(TEXTOID, -1, InvalidOid, -1,
									 PointerGetDatum(databaseDatum), false,
									 false);
	Const *collectionConst = MakeTextConst(collectionName->string,
										   collectionName->length);
	Const *keyConst = MakeTextConst(distinctKey->string, distinctKey->length);

	List *args = list_make3(databaseConst, collectionConst, keyConst);
	FuncExpr *scanExpr = makeFuncExpr(BsonDistinctIndexScanFunctionOid(),
									  BsonTypeId(), args, InvalidOid,
									  InvalidOid, COERCE_EXPLICIT_CALL);

	TargetEntry *entry = makeTargetEntry((Expr *) scanExpr, 1, "document",
										 false);
	query->targetList = list_make1(entry);
	return query;
}
