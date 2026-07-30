/*-------------------------------------------------------------------------
 *
 * rum_enumerate.h
 *	  Streaming enumeration of the distinct entry keys of a RUM index.
 *
 * Portions Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 *-------------------------------------------------------------------------
 */
#ifndef __RUM_ENUMERATE_H__
#define __RUM_ENUMERATE_H__

#include "postgres.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/*
 * Invoked once per visible entry key, in index key order. entryKey points
 * into a transient page copy and must be copied by the callback if kept.
 * Return false to stop the enumeration.
 *
 * This signature is an ABI contract with pg_documentdb's index_am layer
 * (loaded via load_external_function): keep both sides in sync.
 */
typedef bool (*RumEnumerateEntryCallback)(Datum entryKey, int category,
										  void *context);

extern PGDLLEXPORT void RumEnumerateVisibleEntries(Relation indexRel,
												   Relation heapRel,
												   Snapshot snapshot,
												   OffsetNumber attnum,
												   RumEnumerateEntryCallback
												   callback,
												   void *callbackContext);
extern PGDLLEXPORT void documentdb_rum_enumerate_visible_entries(
	Relation indexRel, Relation heapRel, Snapshot snapshot, OffsetNumber attnum,
	RumEnumerateEntryCallback callback, void *callbackContext);

#endif
