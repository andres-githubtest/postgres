/*-------------------------------------------------------------------------
 *
 * sync.c
 *	  File synchronization management code.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/sync/sync.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "access/commit_ts.h"
#include "access/clog.h"
#include "access/multixact.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "postmaster/bgwriter.h"
#include "storage/aio.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"

static MemoryContext pendingOpsCxt; /* context for the pending ops state  */

/*
 * In some contexts (currently, standalone backends and the checkpointer)
 * we keep track of pending fsync operations: we need to remember all relation
 * segments that have been written since the last checkpoint, so that we can
 * fsync them down to disk before completing the next checkpoint.  This hash
 * table remembers the pending operations.  We use a hash table mostly as
 * a convenient way of merging duplicate requests.
 *
 * We use a similar mechanism to remember no-longer-needed files that can
 * be deleted after the next checkpoint, but we use a linked list instead of
 * a hash table, because we don't expect there to be any duplicate requests.
 *
 * These mechanisms are only used for non-temp relations; we never fsync
 * temp rels, nor do we need to postpone their deletion (see comments in
 * mdunlink).
 *
 * (Regular backends do not track pending operations locally, but forward
 * them to the checkpointer.)
 */
typedef uint16 CycleCtr;		/* can be any convenient integer size */

typedef struct PendingFsyncEntry
{
	FileTag		tag;			/* identifies handler and file */
	CycleCtr	cycle_ctr;		/* sync_cycle_ctr of oldest request */
	bool		canceled;		/* canceled is true if we canceled "recently" */
} PendingFsyncEntry;

typedef struct
{
	FileTag		tag;			/* identifies handler and file */
	CycleCtr	cycle_ctr;		/* checkpoint_cycle_ctr when request was made */
} PendingUnlinkEntry;

static HTAB *pendingOps = NULL;
static List *pendingUnlinks = NIL;
static dlist_head inflightSyncs = DLIST_STATIC_INIT(inflightSyncs);
static dlist_head retrySyncs = DLIST_STATIC_INIT(retrySyncs);
static MemoryContext pendingOpsCxt; /* context for the above  */

static CycleCtr sync_cycle_ctr = 0;
static CycleCtr checkpoint_cycle_ctr = 0;

/* Intervals for calling AbsorbSyncRequests */
#define FSYNCS_PER_ABSORB		10
#define UNLINKS_PER_ABSORB		10

/*
 * Function pointers for handling sync and unlink requests.
 */
typedef struct SyncOps
{
	void		(*sync_syncfiletag) (PgStreamingWrite *pgsw, InflightSyncEntry *entry);
	int			(*sync_unlinkfiletag) (const FileTag *ftag, char *path);
	bool		(*sync_filetagmatches) (const FileTag *ftag,
										const FileTag *candidate);
} SyncOps;

/*
 * These indexes must correspond to the values of the SyncRequestHandler enum.
 */
static const SyncOps syncsw[] = {
	/* magnetic disk */
	[SYNC_HANDLER_MD] = {
		.sync_syncfiletag = mdsyncfiletag,
		.sync_unlinkfiletag = mdunlinkfiletag,
		.sync_filetagmatches = mdfiletagmatches
	},
	/* pg_xact */
	[SYNC_HANDLER_CLOG] = {
		.sync_syncfiletag = clogsyncfiletag
	},
	/* pg_commit_ts */
	[SYNC_HANDLER_COMMIT_TS] = {
		.sync_syncfiletag = committssyncfiletag
	},
	/* pg_multixact/offsets */
	[SYNC_HANDLER_MULTIXACT_OFFSET] = {
		.sync_syncfiletag = multixactoffsetssyncfiletag
	},
	/* pg_multixact/members */
	[SYNC_HANDLER_MULTIXACT_MEMBER] = {
		.sync_syncfiletag = multixactmemberssyncfiletag
	}
};

/*
 * Initialize data structures for the file sync tracking.
 */
void
InitSync(void)
{
	/*
	 * Create pending-operations hashtable if we need it.  Currently, we need
	 * it if we are standalone (not under a postmaster) or if we are a
	 * checkpointer auxiliary process.
	 */
	if (!IsUnderPostmaster || AmCheckpointerProcess())
	{
		HASHCTL		hash_ctl;

		/*
		 * XXX: The checkpointer needs to add entries to the pending ops table
		 * when absorbing fsync requests.  That is done within a critical
		 * section, which isn't usually allowed, but we make an exception. It
		 * means that there's a theoretical possibility that you run out of
		 * memory while absorbing fsync requests, which leads to a PANIC.
		 * Fortunately the hash table is small so that's unlikely to happen in
		 * practice.
		 */
		pendingOpsCxt = AllocSetContextCreate(TopMemoryContext,
											  "Pending ops context",
											  ALLOCSET_DEFAULT_SIZES);
		MemoryContextAllowInCriticalSection(pendingOpsCxt, true);

		hash_ctl.keysize = sizeof(FileTag);
		hash_ctl.entrysize = sizeof(PendingFsyncEntry);
		hash_ctl.hcxt = pendingOpsCxt;
		pendingOps = hash_create("Pending Ops Table",
								 100L,
								 &hash_ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		pendingUnlinks = NIL;
	}

}

/*
 * SyncPreCheckpoint() -- Do pre-checkpoint work
 *
 * To distinguish unlink requests that arrived before this checkpoint
 * started from those that arrived during the checkpoint, we use a cycle
 * counter similar to the one we use for fsync requests. That cycle
 * counter is incremented here.
 *
 * This must be called *before* the checkpoint REDO point is determined.
 * That ensures that we won't delete files too soon.
 *
 * Note that we can't do anything here that depends on the assumption
 * that the checkpoint will be completed.
 */
void
SyncPreCheckpoint(void)
{
	/*
	 * Any unlink requests arriving after this point will be assigned the next
	 * cycle counter, and won't be unlinked until next checkpoint.
	 */
	checkpoint_cycle_ctr++;
}

/*
 * SyncPostCheckpoint() -- Do post-checkpoint work
 *
 * Remove any lingering files that can now be safely removed.
 */
void
SyncPostCheckpoint(void)
{
	int			absorb_counter;

	absorb_counter = UNLINKS_PER_ABSORB;
	while (pendingUnlinks != NIL)
	{
		PendingUnlinkEntry *entry = (PendingUnlinkEntry *) linitial(pendingUnlinks);
		char		path[MAXPGPATH];

		/*
		 * New entries are appended to the end, so if the entry is new we've
		 * reached the end of old entries.
		 *
		 * Note: if just the right number of consecutive checkpoints fail, we
		 * could be fooled here by cycle_ctr wraparound.  However, the only
		 * consequence is that we'd delay unlinking for one more checkpoint,
		 * which is perfectly tolerable.
		 */
		if (entry->cycle_ctr == checkpoint_cycle_ctr)
			break;

		/* Unlink the file */
		if (syncsw[entry->tag.handler].sync_unlinkfiletag(&entry->tag,
														  path) < 0)
		{
			/*
			 * There's a race condition, when the database is dropped at the
			 * same time that we process the pending unlink requests. If the
			 * DROP DATABASE deletes the file before we do, we will get ENOENT
			 * here. rmtree() also has to ignore ENOENT errors, to deal with
			 * the possibility that we delete the file first.
			 */
			if (errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
		}

		/* And remove the list entry */
		pendingUnlinks = list_delete_first(pendingUnlinks);
		pfree(entry);

		/*
		 * As in ProcessSyncRequests, we don't want to stop absorbing fsync
		 * requests for a long time when there are many deletions to be done.
		 * We can safely call AbsorbSyncRequests() at this point in the loop
		 * (note it might try to delete list entries).
		 */
		if (--absorb_counter <= 0)
		{
			AbsorbSyncRequests();
			absorb_counter = UNLINKS_PER_ABSORB;
		}
	}
}

typedef struct SyncState
{
	bool		sync_in_progress;
	int			absorb_counter;
	PgStreamingWrite *pgsw;

	/* stats */
	int			processed;
	uint64		longest;
	uint64		total_elapsed;
} SyncState;
static SyncState sync_state_global = {.sync_in_progress = false};

void
SyncRequestCompleted(InflightSyncEntry *inflight_entry, bool success, int err)
{
	/*
	 * XXX: we could easily transport this as part of InflightSyncEntry, or
	 * specify it in the callers. But right now that's not really needed.
	 */
	SyncState *sync_state = &sync_state_global;

	dlist_delete_from(&inflightSyncs, &inflight_entry->node);

	if (success)
	{
		instr_time now;
		uint64 elapsed;

		/* Success; update statistics about sync timing */

		/*
		 * XXX: These stats are pretty useless right now, with AIO many IOs
		 * may be in process at the same time, and but we'll measure from IO
		 * submission to IO completion reception.
		 */
		INSTR_TIME_SET_CURRENT(now);
		elapsed = INSTR_TIME_GET_MICROSEC(now) - inflight_entry->start_time;

		if (elapsed > sync_state->longest)
			sync_state->longest =elapsed;
		sync_state->total_elapsed += elapsed;
		sync_state->processed++;

		if (log_checkpoints)
			elog(DEBUG1, "checkpoint sync: number=%d file=%s time=%.3f ms",
				 sync_state->processed,
				 inflight_entry->path,
				 (double) elapsed / 1000);

		/*
		 * We are done with this entry, remove it.
		 *
		 * Normally it is not safe to HASH_REMOVE entries other than the
		 * current element while iterating, but it ought to be safe because it
		 * is guaranteed to be an element from earlier in the iteration.
		 */
		if (hash_search(pendingOps, &inflight_entry->hash_entry->tag, HASH_REMOVE, NULL) == NULL)
			elog(ERROR, "pendingOps corrupted");

		pfree(inflight_entry);
	}
	else
	{
		/*
		 * It is possible that the relation has been dropped or truncated
		 * since the fsync request was entered. Therefore, allow ENOENT, but
		 * only if we didn't fail already on this file.
		 */
		errno = err;
		if (!FILE_POSSIBLY_DELETED(errno) || inflight_entry->retry_count > 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m",
							inflight_entry->path)));
		else
			ereport(DEBUG1,
					(errcode_for_file_access(),
					 errmsg_internal("could not fsync file \"%s\" but retrying: %m",
									 inflight_entry->path)));

		/* schedule request to be retried */
		inflight_entry->retry_count++;
		dlist_push_tail(&retrySyncs, &inflight_entry->node);
	}
}

static void
call_syncfiletag(SyncState *sync_state, InflightSyncEntry *inflight_entry)
{
	instr_time now;

	dlist_push_tail(&inflightSyncs, &inflight_entry->node);

	INSTR_TIME_SET_CURRENT(now);
	inflight_entry->start_time = INSTR_TIME_GET_MICROSEC(now);
	syncsw[inflight_entry->tag.handler].sync_syncfiletag(sync_state->pgsw, inflight_entry);
}

/*
 * The fsync table could contain requests to fsync segments that
 * have been deleted (unlinked) by the time we get to them. Rather
 * than just hoping an ENOENT (or EACCES on Windows) error can be
 * ignored, what we do on error is absorb pending requests and
 * then retry. Since mdunlink() queues a "cancel" message before
 * actually unlinking, the fsync request is guaranteed to be
 * marked canceled after the absorb if it really was this case.
 * DROP DATABASE likewise has to tell us to forget fsync requests
 * before it starts deletions.
 */
static void
RetrySyncRequests(SyncState *sync_state)
{
	if (likely(dlist_is_empty(&retrySyncs)))
		return;

	/*
	 * Absorb incoming requests and check to see if a cancel
	 * arrived for this relation fork.
	 */
	AbsorbSyncRequests();
	sync_state->absorb_counter = FSYNCS_PER_ABSORB; /* might as well... */

	while (!dlist_is_empty(&retrySyncs))
	{
		dlist_node *node = dlist_pop_head_node(&retrySyncs);
		InflightSyncEntry *inflight_entry = dlist_container(InflightSyncEntry, node, node);

		if (inflight_entry->hash_entry->canceled)
		{
			if (hash_search(pendingOps, &inflight_entry->hash_entry->tag, HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "pendingOps corrupted");
			continue;
		}

		Assert(inflight_entry->retry_count <= 5);

		call_syncfiletag(sync_state, inflight_entry);
	}
}

/*
 *	ProcessSyncRequests() -- Process queued fsync requests.
 */
void
ProcessSyncRequests(void)
{
	HASH_SEQ_STATUS hstat;
	PendingFsyncEntry *entry;
	SyncState *sync_state = &sync_state_global;

	/*
	 * This is only called during checkpoints, and checkpoints should only
	 * occur in processes that have created a pendingOps.
	 */
	if (!pendingOps)
		elog(ERROR, "cannot sync without a pendingOps table");

	/*
	 * If we are in the checkpointer, the sync had better include all fsync
	 * requests that were queued by backends up to this point.  The tightest
	 * race condition that could occur is that a buffer that must be written
	 * and fsync'd for the checkpoint could have been dumped by a backend just
	 * before it was visited by BufferSync().  We know the backend will have
	 * queued an fsync request before clearing the buffer's dirtybit, so we
	 * are safe as long as we do an Absorb after completing BufferSync().
	 */
	AbsorbSyncRequests();

	/*
	 * To avoid excess fsync'ing (in the worst case, maybe a never-terminating
	 * checkpoint), we want to ignore fsync requests that are entered into the
	 * hashtable after this point --- they should be processed next time,
	 * instead.  We use sync_cycle_ctr to tell old entries apart from new
	 * ones: new ones will have cycle_ctr equal to the incremented value of
	 * sync_cycle_ctr.
	 *
	 * In normal circumstances, all entries present in the table at this point
	 * will have cycle_ctr exactly equal to the current (about to be old)
	 * value of sync_cycle_ctr.  However, if we fail partway through the
	 * fsync'ing loop, then older values of cycle_ctr might remain when we
	 * come back here to try again.  Repeated checkpoint failures would
	 * eventually wrap the counter around to the point where an old entry
	 * might appear new, causing us to skip it, possibly allowing a checkpoint
	 * to succeed that should not have.  To forestall wraparound, any time the
	 * previous ProcessSyncRequests() failed to complete, run through the
	 * table and forcibly set cycle_ctr = sync_cycle_ctr.
	 *
	 * Think not to merge this loop with the main loop, as the problem is
	 * exactly that that loop may fail before having visited all the entries.
	 * From a performance point of view it doesn't matter anyway, as this path
	 * will never be taken in a system that's functioning normally.
	 */
	if (sync_state->sync_in_progress)
	{
		/*
		 * FIXME: SyncState, referenced by potential in-flight requests will
		 * be referenced. Nor are we correctly dealing with retrySyncs,
		 * inflightSyncs.
		 */
		elog(PANIC, "not implemented right now");

		/* prior try failed, so update any stale cycle_ctr values */
		hash_seq_init(&hstat, pendingOps);
		while ((entry = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
		{
			entry->cycle_ctr = sync_cycle_ctr;
		}
	}

	sync_state->processed = 0;
	sync_state->longest = 0;
	sync_state->total_elapsed = 0;

	if (!sync_state->pgsw)
	{
		MemoryContext old_context = MemoryContextSwitchTo(pendingOpsCxt);

		sync_state->pgsw = pg_streaming_write_alloc(128, &sync_state);

		MemoryContextSwitchTo(old_context);
	}

	/* Advance counter so that new hashtable entries are distinguishable */
	sync_cycle_ctr++;

	/* Set flag to detect failure if we don't reach the end of the loop */
	sync_state->sync_in_progress = true;

	/* Now scan the hashtable for fsync requests to process */
	sync_state->absorb_counter = FSYNCS_PER_ABSORB;
	hash_seq_init(&hstat, pendingOps);
	while ((entry = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
	{
		/*
		 * If the entry is new then don't process it this time; it is new.
		 * Note "continue" bypasses the hash-remove call at the bottom of the
		 * loop.
		 */
		if (entry->cycle_ctr == sync_cycle_ctr)
			continue;

		/* Else assert we haven't missed it */
		Assert((CycleCtr) (entry->cycle_ctr + 1) == sync_cycle_ctr);

		/*
		 * If in checkpointer, we want to absorb pending requests every so
		 * often to prevent overflow of the fsync request queue.  It is
		 * unspecified whether newly-added entries will be visited by
		 * hash_seq_search, but we don't care since we don't need to
		 * process them anyway.
		 */
		if (--sync_state->absorb_counter <= 0)
		{
			AbsorbSyncRequests();
			sync_state->absorb_counter = FSYNCS_PER_ABSORB;
		}

		/*
		 * If fsync is off then we don't have to bother opening the file at
		 * all.  (We delay checking until this point so that changing fsync on
		 * the fly behaves sensibly.)
		 */
		if (!enableFsync || entry->canceled)
		{
			/* We are done with this entry, remove it */
			if (hash_search(pendingOps, &entry->tag, HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "pendingOps corrupted");
		}
		else
		{
			InflightSyncEntry *inflight_entry;

			inflight_entry = MemoryContextAlloc(pendingOpsCxt,
												sizeof(InflightSyncEntry));
			inflight_entry->tag = entry->tag;
			inflight_entry->handler_data = 0;
			inflight_entry->path[0] = 0;
			inflight_entry->retry_count = 0;
			inflight_entry->hash_entry = entry;

			call_syncfiletag(sync_state, inflight_entry);
		}
	}							/* end loop over hashtable entries */

	pg_streaming_write_wait_all(sync_state->pgsw);

	for (int failures = 0; failures < 5; failures++)
	{
		RetrySyncRequests(sync_state);
	}

	if (!dlist_is_empty(&inflightSyncs))
		elog(PANIC, "inflight sync requests corrupted");
	if (!dlist_is_empty(&retrySyncs))
		elog(PANIC, "inflight sync requests corrupted");
	pg_streaming_write_free(sync_state->pgsw);
	sync_state->pgsw = NULL;

	/* Return sync performance metrics for report at checkpoint end */
	CheckpointStats.ckpt_sync_rels = sync_state->processed;
	CheckpointStats.ckpt_longest_sync = sync_state->longest;
	CheckpointStats.ckpt_agg_sync_time = sync_state->total_elapsed;

	/* Flag successful completion of ProcessSyncRequests */
	sync_state->sync_in_progress = false;
}

/*
 * RememberSyncRequest() -- callback from checkpointer side of sync request
 *
 * We stuff fsync requests into the local hash table for execution
 * during the checkpointer's next checkpoint.  UNLINK requests go into a
 * separate linked list, however, because they get processed separately.
 *
 * See sync.h for more information on the types of sync requests supported.
 */
void
RememberSyncRequest(const FileTag *ftag, SyncRequestType type)
{
	Assert(pendingOps);

	if (type == SYNC_FORGET_REQUEST)
	{
		PendingFsyncEntry *entry;

		/* Cancel previously entered request */
		entry = (PendingFsyncEntry *) hash_search(pendingOps,
												  (void *) ftag,
												  HASH_FIND,
												  NULL);
		if (entry != NULL)
			entry->canceled = true;
	}
	else if (type == SYNC_FILTER_REQUEST)
	{
		HASH_SEQ_STATUS hstat;
		PendingFsyncEntry *entry;
		ListCell   *cell;

		/* Cancel matching fsync requests */
		hash_seq_init(&hstat, pendingOps);
		while ((entry = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
		{
			if (entry->tag.handler == ftag->handler &&
				syncsw[ftag->handler].sync_filetagmatches(ftag, &entry->tag))
				entry->canceled = true;
		}

		/* Remove matching unlink requests */
		foreach(cell, pendingUnlinks)
		{
			PendingUnlinkEntry *entry = (PendingUnlinkEntry *) lfirst(cell);

			if (entry->tag.handler == ftag->handler &&
				syncsw[ftag->handler].sync_filetagmatches(ftag, &entry->tag))
			{
				pendingUnlinks = foreach_delete_current(pendingUnlinks, cell);
				pfree(entry);
			}
		}
	}
	else if (type == SYNC_UNLINK_REQUEST)
	{
		/* Unlink request: put it in the linked list */
		MemoryContext oldcxt = MemoryContextSwitchTo(pendingOpsCxt);
		PendingUnlinkEntry *entry;

		entry = palloc(sizeof(PendingUnlinkEntry));
		entry->tag = *ftag;
		entry->cycle_ctr = checkpoint_cycle_ctr;

		pendingUnlinks = lappend(pendingUnlinks, entry);

		MemoryContextSwitchTo(oldcxt);
	}
	else
	{
		/* Normal case: enter a request to fsync this segment */
		MemoryContext oldcxt = MemoryContextSwitchTo(pendingOpsCxt);
		PendingFsyncEntry *entry;
		bool		found;

		Assert(type == SYNC_REQUEST);

		entry = (PendingFsyncEntry *) hash_search(pendingOps,
												  (void *) ftag,
												  HASH_ENTER,
												  &found);
		/* if new entry, or was previously canceled, initialize it */
		if (!found || entry->canceled)
		{
			entry->cycle_ctr = sync_cycle_ctr;
			entry->canceled = false;
		}

		/*
		 * NB: it's intentional that we don't change cycle_ctr if the entry
		 * already exists.  The cycle_ctr must represent the oldest fsync
		 * request that could be in the entry.
		 */

		MemoryContextSwitchTo(oldcxt);
	}
}

/*
 * Register the sync request locally, or forward it to the checkpointer.
 *
 * If retryOnError is true, we'll keep trying if there is no space in the
 * queue.  Return true if we succeeded, or false if there wasn't space.
 */
bool
RegisterSyncRequest(const FileTag *ftag, SyncRequestType type,
					bool retryOnError)
{
	bool		ret;

	if (pendingOps != NULL)
	{
		/* standalone backend or startup process: fsync state is local */
		RememberSyncRequest(ftag, type);
		return true;
	}

	for (;;)
	{
		/*
		 * Notify the checkpointer about it.  If we fail to queue a message in
		 * retryOnError mode, we have to sleep and try again ... ugly, but
		 * hopefully won't happen often.
		 *
		 * XXX should we CHECK_FOR_INTERRUPTS in this loop?  Escaping with an
		 * error in the case of SYNC_UNLINK_REQUEST would leave the
		 * no-longer-used file still present on disk, which would be bad, so
		 * I'm inclined to assume that the checkpointer will always empty the
		 * queue soon.
		 */
		ret = ForwardSyncRequest(ftag, type);

		/*
		 * If we are successful in queueing the request, or we failed and were
		 * instructed not to retry on error, break.
		 */
		if (ret || (!ret && !retryOnError))
			break;

		pg_usleep(10000L);
	}

	return ret;
}
