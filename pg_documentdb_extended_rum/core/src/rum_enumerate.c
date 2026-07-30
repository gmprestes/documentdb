/*-------------------------------------------------------------------------
 *
 * rum_enumerate.c
 *	  Streaming enumeration of the distinct entry keys of a RUM index.
 *
 * Portions Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * Walks the entry tree of one attribute from its first key to its last,
 * invoking a callback once per entry that has at least one heap-visible
 * posting item. This gives an O(distinct values) primitive for answering
 * "distinct" style questions from the index without scanning one tuple
 * per document.
 *
 * Correctness against concurrent activity:
 *
 * - Entry pages are processed from a local copy taken under a share lock
 *   (the same discipline the ordered scan uses, see
 *   MoveBuffersForOrderedScan in rumget.c). A pin is kept on the real
 *   page while its copy is being processed, which prevents the page from
 *   being deleted; if the page splits meanwhile, the entries that moved
 *   right are already present in the copy, and the resulting right-link
 *   mismatch is resolved by skipping entries up to the last processed
 *   key on the next page.
 *
 * - The entry tree contains keys for dead and not-yet-visible tuples
 *   until vacuum removes them, so a key is only reported after at least
 *   one posting item passes a visibility check: the page's all-visible
 *   bit answers most items without heap access, and the remainder go
 *   through table_index_fetch_tuple with the caller's snapshot, which
 *   resolves HOT chains exactly like an index-only scan's heap recheck.
 *   Entries with no visible posting items are suppressed.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "pg_documentdb_rum.h"
#include "rum_enumerate.h"

/* Local mirror of rumget.c's CopyPageContents (static there). */
static void
CopyPageContentsForEnumerate(Page targetPage, Page sourcePage)
{
	Size pageSize = PageGetPageSize(sourcePage);
	memcpy(targetPage, sourcePage, pageSize);
}


/* Shared state for the heap visibility checks of one enumeration. */
typedef struct RumEnumerateVisibilityState
{
	Relation heapRel;
	Snapshot snapshot;
	Buffer vmBuffer;
	IndexFetchTableData *heapFetch;
	TupleTableSlot *slot;
} RumEnumerateVisibilityState;


/*
 * True when the heap tuple (or a member of its HOT chain) at tid is visible
 * to the snapshot. The visibility map answers all-visible pages without
 * touching the heap.
 */
static bool
TidHasVisibleTuple(RumEnumerateVisibilityState *visState, ItemPointer tid)
{
	if (VM_ALL_VISIBLE(visState->heapRel, ItemPointerGetBlockNumber(tid),
					   &visState->vmBuffer))
	{
		return true;
	}

	ItemPointerData heapTid = *tid;
	bool callAgain = false;
	bool allDead = false;

	bool found = table_index_fetch_tuple(visState->heapFetch, &heapTid,
										 visState->snapshot, visState->slot,
										 &callAgain, &allDead);
	while (!found && callAgain)
	{
		found = table_index_fetch_tuple(visState->heapFetch, &heapTid,
										visState->snapshot, visState->slot,
										&callAgain, &allDead);
	}

	if (found)
	{
		ExecClearTuple(visState->slot);
	}

	return found;
}


/*
 * True when at least one posting item of the entry (inline posting list or
 * posting tree) is visible to the snapshot. Stops at the first visible item.
 * itup points into a local page copy, so no lock is required to decode it.
 */
static bool
EntryHasVisiblePostingItem(RumState *rumstate, OffsetNumber attnum,
						   IndexTuple itup,
						   RumEnumerateVisibilityState *visState)
{
	if (!RumIsPostingTree(itup))
	{
		int nposting = RumGetNPosting(itup);
		if (nposting <= 0)
		{
			return false;
		}

		RumItem *items = (RumItem *) palloc(sizeof(RumItem) * nposting);
		bool copyAddInfo = false;
		rumReadTuple(rumstate, attnum, itup, items, copyAddInfo);

		bool found = false;
		for (int i = 0; i < nposting; i++)
		{
			if (TidHasVisibleTuple(visState, &items[i].iptr))
			{
				found = true;
				break;
			}
		}

		pfree(items);
		return found;
	}

	/* Posting tree: walk the leaf data pages from the left. */
	BlockNumber rootBlkno = RumGetPostingTree(itup);
	bool searchMode = true;
	RumPostingTreeScan *gdi = rumPrepareScanPostingTree(
		rumstate->index, rootBlkno, searchMode, ForwardScanDirection, attnum,
		rumstate);

	Buffer buffer = rumScanBeginPostingTree(gdi, NULL);

	/* Own an extra pin: rumStep consumes one pin per advance while the
	 * stack keeps its own reference to the leftmost leaf (same pattern as
	 * the entry scan in rumget.c).
	 */
	IncrBufferRefCount(buffer);

	/* Same per-page item bound the entry scan assumes (see startScan's
	 * BLCKSZ-sized RumItem list in rumget.c).
	 */
	bool found = false;
	ItemPointerData *tids = (ItemPointerData *) palloc(
		sizeof(ItemPointerData) * BLCKSZ);

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		Page page = BufferGetPage(buffer);
		OffsetNumber maxoff = RumDataPageMaxOff(page);
		int ntids = 0;

		if (!RumPageIsDeleted(page))
		{
			Pointer ptr = RumDataPageGetData(page);
			RumItem item;
			ItemPointerSetMin(&item.iptr);
			for (OffsetNumber off = FirstOffsetNumber; off <= maxoff;
				 off = OffsetNumberNext(off))
			{
				bool copyAddInfo = false;
				ptr = rumDataPageLeafRead(ptr, attnum, &item, copyAddInfo,
										  rumstate);
				tids[ntids++] = item.iptr;
			}
		}

		bool isRightMost = RumPageRightMost(page);

		/* Check visibility without holding the page lock (pin retained). */
		LockBuffer(buffer, RUM_UNLOCK);
		for (int i = 0; i < ntids && !found; i++)
		{
			found = TidHasVisibleTuple(visState, &tids[i]);
		}

		if (found || isRightMost)
		{
			ReleaseBuffer(buffer);
			break;
		}

		LockBuffer(buffer, RUM_SHARE);
		buffer = rumStep(buffer, rumstate->index, RUM_SHARE,
						 ForwardScanDirection);
		if (!BufferIsValid(buffer))
		{
			break;
		}
	}

	pfree(tids);
	freeRumBtreeStack(gdi->stack);
	pfree(gdi);
	return found;
}


/*
 * Enumerates the distinct entry keys of attnum that have at least one
 * heap-visible posting item, in index key order, invoking callback once per
 * key. The key datum passed to the callback points into a transient page
 * copy: the callback must copy anything it wants to keep. Enumeration stops
 * early when the callback returns false.
 */
PGDLLEXPORT void
RumEnumerateVisibleEntries(Relation indexRel, Relation heapRel,
						   Snapshot snapshot, OffsetNumber attnum,
						   RumEnumerateEntryCallback callback,
						   void *callbackContext)
{
	RumState rumstate;
	initRumState(&rumstate, indexRel);

	RumEnumerateVisibilityState visState = { 0 };
	visState.heapRel = heapRel;
	visState.snapshot = snapshot;
	visState.vmBuffer = InvalidBuffer;
	visState.heapFetch = table_index_fetch_begin(heapRel);
	visState.slot = table_slot_create(heapRel, NULL);

	/* This is semantically a scan of every row of the collection. */
	PredicateLockRelation(heapRel, snapshot);

	/* Position on the first entry page that can hold this attribute's keys:
	 * RUM_CAT_EMPTY_QUERY sorts before every key of the attribute.
	 */
	RumBtreeData btree;
	rumPrepareEntryScan(&btree, attnum, (Datum) 0, RUM_CAT_EMPTY_QUERY,
						&rumstate);
	btree.searchMode = true;
	btree.fullScan = true;

	RumBtreeStack *stack = rumFindLeafPage(&btree, NULL);

	PageHeaderData *pageCopy = (PageHeaderData *) palloc(BLCKSZ);
	IndexTuple lastProcessedTuple = NULL;
	bool stopped = false;

	/* rumFindLeafPage returns the leaf locked and pinned; take the first
	 * copy under that lock.
	 */
	CopyPageContentsForEnumerate((Page) pageCopy,
								 BufferGetPage(stack->buffer));
	PredicateLockPage(indexRel, BufferGetBlockNumber(stack->buffer), snapshot);
	LockBuffer(stack->buffer, RUM_UNLOCK);

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		BlockNumber expectedNext =
			RumPageGetOpaque((Page) pageCopy)->rightlink;
		OffsetNumber maxoff = PageGetMaxOffsetNumber((Page) pageCopy);
		bool pageDeleted = RumPageIsDeleted((Page) pageCopy);
		OffsetNumber off;

		for (off = FirstOffsetNumber; !pageDeleted && off <= maxoff;
			 off = OffsetNumberNext(off))
		{
			IndexTuple itup = (IndexTuple) PageGetItem(
				(Page) pageCopy, PageGetItemId((Page) pageCopy, off));

			OffsetNumber itupAttnum = rumtuple_get_attrnum(&rumstate, itup);
			if (itupAttnum < attnum)
			{
				continue;
			}

			if (itupAttnum > attnum)
			{
				stopped = true;
				break;
			}

			RumNullCategory category;
			Datum key = rumtuple_get_key(&rumstate, itup, &category);

			if (lastProcessedTuple != NULL)
			{
				/* Resuming after a page split: skip keys already seen. */
				RumNullCategory lastCategory;
				Datum lastKey = rumtuple_get_key(&rumstate,
												 lastProcessedTuple,
												 &lastCategory);
				if (rumCompareAttEntries(&rumstate, itupAttnum, key, category,
										 attnum, lastKey, lastCategory) <= 0)
				{
					continue;
				}

				pfree(lastProcessedTuple);
				lastProcessedTuple = NULL;
			}

			/* Categories other than regular keys (null markers, the full
			 * scan placeholder) are not user values.
			 */
			if (category != RUM_CAT_NORM_KEY)
			{
				continue;
			}

			if (!EntryHasVisiblePostingItem(&rumstate, attnum, itup,
											&visState))
			{
				continue;
			}

			if (!callback(key, category, callbackContext))
			{
				stopped = true;
				break;
			}
		}

		if (stopped)
		{
			break;
		}

		/* Remember the last tuple of the processed copy so a concurrent
		 * split (detected below as a right-link mismatch) does not make us
		 * process the moved entries twice.
		 */
		if (maxoff >= FirstOffsetNumber && !pageDeleted)
		{
			IndexTuple lastTuple = (IndexTuple) PageGetItem(
				(Page) pageCopy, PageGetItemId((Page) pageCopy, maxoff));
			if (rumtuple_get_attrnum(&rumstate, lastTuple) == attnum)
			{
				if (lastProcessedTuple != NULL)
				{
					pfree(lastProcessedTuple);
				}
				lastProcessedTuple = CopyIndexTuple(lastTuple);
			}
		}

		LockBuffer(stack->buffer, RUM_SHARE);
		if (RumPageRightMost(BufferGetPage(stack->buffer)))
		{
			LockBuffer(stack->buffer, RUM_UNLOCK);
			break;
		}

		stack->buffer = rumStep(stack->buffer, indexRel, RUM_SHARE,
								ForwardScanDirection);
		stack->blkno = BufferGetBlockNumber(stack->buffer);

		if (stack->blkno == expectedNext && lastProcessedTuple != NULL)
		{
			/* No split since the copy was taken - no need to dedup. */
			pfree(lastProcessedTuple);
			lastProcessedTuple = NULL;
		}

		CopyPageContentsForEnumerate((Page) pageCopy,
									 BufferGetPage(stack->buffer));
		PredicateLockPage(indexRel, stack->blkno, snapshot);
		LockBuffer(stack->buffer, RUM_UNLOCK);
	}

	if (lastProcessedTuple != NULL)
	{
		pfree(lastProcessedTuple);
	}

	pfree(pageCopy);
	freeRumBtreeStack(stack);

	if (visState.vmBuffer != InvalidBuffer)
	{
		ReleaseBuffer(visState.vmBuffer);
	}

	ExecDropSingleTupleTableSlot(visState.slot);
	table_index_fetch_end(visState.heapFetch);
}


/*
 * Catalog-name wrapper: pg_documentdb resolves this symbol from the core
 * library via load_external_function (see RumFunctionCatalog there).
 */
PGDLLEXPORT void
documentdb_rum_enumerate_visible_entries(Relation indexRel, Relation heapRel,
										 Snapshot snapshot, OffsetNumber attnum,
										 RumEnumerateEntryCallback callback,
										 void *callbackContext)
{
	RumEnumerateVisibleEntries(indexRel, heapRel, snapshot, attnum, callback,
							   callbackContext);
}
