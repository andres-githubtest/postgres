/* -------------------------------------------------------------------------
 *
 * pgstat_global.c
 *	  Implementation of all global statistics.
 *
 * This file contains the implementation of global statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Copyright (c) 2001-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_global.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/instrument.h"
#include "replication/slot.h"
#include "utils/builtins.h"
#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


/* ----------
 * pending stats state that is directly modified from outside the stats system
 * ----------
 */

/*
 * Stored directly in a stats message structure so they can be sent
 * without needing to copy things around.  We assume these init to zeroes.
 */
PgStat_MsgBgWriter PendingBgWriterStats;
PgStat_MsgCheckpointer PendingCheckpointerStats;
PgStat_MsgWal WalStats;

/*
 * SLRU statistics counts waiting to be sent to the collector.  These are
 * stored directly in stats message format so they can be sent without needing
 * to copy things around.  We assume this variable inits to zeroes.  Entries
 * are one-to-one with slru_names[].
 */
static PgStat_MsgSLRU SLRUStats[SLRU_NUM_ELEMENTS];

/*
 * WAL usage counters saved from pgWALUsage at the previous call to
 * pgstat_send_wal(). This is used to calculate how much WAL usage
 * happens between pgstat_send_wal() calls, by subtracting
 * the previous counters from the current ones.
 *
 * FIXME: It'd be better if this weren't global.
 */
WalUsage prevWalUsage;


/* ----------
 * pgstat_reset_shared_counters() -
 *
 *	Tell the statistics collector to reset cluster-wide shared counters.
 *
 *	Permission checking for this function is managed through the normal
 *	GRANT system.
 * ----------
 */
void
pgstat_reset_shared_counters(const char *target)
{
	PgStat_MsgResetsharedcounter msg;

	if (pgStatSock == PGINVALID_SOCKET)
		return;

	if (strcmp(target, "archiver") == 0)
		msg.m_resettarget = RESET_ARCHIVER;
	else if (strcmp(target, "bgwriter") == 0)
		msg.m_resettarget = RESET_BGWRITER;
	else if (strcmp(target, "wal") == 0)
		msg.m_resettarget = RESET_WAL;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized reset target: \"%s\"", target),
				 errhint("Target must be \"archiver\", \"bgwriter\", or \"wal\".")));

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_RESETSHAREDCOUNTER);
	pgstat_send(&msg, sizeof(msg));
}

/* ----------
 * pgstat_send_archiver() -
 *
 *	Tell the collector about the WAL file that we successfully
 *	archived or failed to archive.
 * ----------
 */
void
pgstat_send_archiver(const char *xlog, bool failed)
{
	PgStat_MsgArchiver msg;

	/*
	 * Prepare and send the message
	 */
	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_ARCHIVER);
	msg.m_failed = failed;
	strlcpy(msg.m_xlog, xlog, sizeof(msg.m_xlog));
	msg.m_timestamp = GetCurrentTimestamp();
	pgstat_send(&msg, sizeof(msg));
}

/* ----------
 * pgstat_send_bgwriter() -
 *
 *		Send bgwriter statistics to the collector
 * ----------
 */
void
pgstat_send_bgwriter(void)
{
	/* We assume this initializes to zeroes */
	static const PgStat_MsgBgWriter all_zeroes;

	pgstat_assert_is_up();

	/*
	 * This function can be called even if nothing at all has happened. In
	 * this case, avoid sending a completely empty message to the stats
	 * collector.
	 */
	if (memcmp(&PendingBgWriterStats, &all_zeroes, sizeof(PgStat_MsgBgWriter)) == 0)
		return;

	/*
	 * Prepare and send the message
	 */
	pgstat_setheader(&PendingBgWriterStats.m_hdr, PGSTAT_MTYPE_BGWRITER);
	pgstat_send(&PendingBgWriterStats, sizeof(PendingBgWriterStats));

	/*
	 * Clear out the statistics buffer, so it can be re-used.
	 */
	MemSet(&PendingBgWriterStats, 0, sizeof(PendingBgWriterStats));
}

/* ----------
 * pgstat_send_checkpointer() -
 *
 *		Send checkpointer statistics to the collector
 * ----------
 */
void
pgstat_send_checkpointer(void)
{
	/* We assume this initializes to zeroes */
	static const PgStat_MsgCheckpointer all_zeroes;

	/*
	 * This function can be called even if nothing at all has happened. In
	 * this case, avoid sending a completely empty message to the stats
	 * collector.
	 */
	if (memcmp(&PendingCheckpointerStats, &all_zeroes, sizeof(PgStat_MsgCheckpointer)) == 0)
		return;

	/*
	 * Prepare and send the message
	 */
	pgstat_setheader(&PendingCheckpointerStats.m_hdr, PGSTAT_MTYPE_CHECKPOINTER);
	pgstat_send(&PendingCheckpointerStats, sizeof(PendingCheckpointerStats));

	/*
	 * Clear out the statistics buffer, so it can be re-used.
	 */
	MemSet(&PendingCheckpointerStats, 0, sizeof(PendingCheckpointerStats));
}

/* ----------
 * pgstat_report_replslot() -
 *
 *	Tell the collector about replication slot statistics.
 * ----------
 */
void
pgstat_report_replslot(const PgStat_StatReplSlotEntry *repSlotStat)
{
	PgStat_MsgReplSlot msg;

	/*
	 * Prepare and send the message
	 */
	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_REPLSLOT);
	namestrcpy(&msg.m_slotname, NameStr(repSlotStat->slotname));
	msg.m_create = false;
	msg.m_drop = false;
	msg.m_spill_txns = repSlotStat->spill_txns;
	msg.m_spill_count = repSlotStat->spill_count;
	msg.m_spill_bytes = repSlotStat->spill_bytes;
	msg.m_stream_txns = repSlotStat->stream_txns;
	msg.m_stream_count = repSlotStat->stream_count;
	msg.m_stream_bytes = repSlotStat->stream_bytes;
	msg.m_total_txns = repSlotStat->total_txns;
	msg.m_total_bytes = repSlotStat->total_bytes;
	pgstat_send(&msg, sizeof(PgStat_MsgReplSlot));
}

/* ----------
 * pgstat_report_replslot_create() -
 *
 *	Tell the collector about creating the replication slot.
 * ----------
 */
void
pgstat_report_replslot_create(const char *slotname)
{
	PgStat_MsgReplSlot msg;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_REPLSLOT);
	namestrcpy(&msg.m_slotname, slotname);
	msg.m_create = true;
	msg.m_drop = false;
	pgstat_send(&msg, sizeof(PgStat_MsgReplSlot));
}

/* ----------
 * pgstat_report_replslot_drop() -
 *
 *	Tell the collector about dropping the replication slot.
 * ----------
 */
void
pgstat_report_replslot_drop(const char *slotname)
{
	PgStat_MsgReplSlot msg;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_REPLSLOT);
	namestrcpy(&msg.m_slotname, slotname);
	msg.m_create = false;
	msg.m_drop = true;
	pgstat_send(&msg, sizeof(PgStat_MsgReplSlot));
}

/* ----------
 * pgstat_reset_replslot_counter() -
 *
 *	Tell the statistics collector to reset a single replication slot
 *	counter, or all replication slots counters (when name is null).
 *
 *	Permission checking for this function is managed through the normal
 *	GRANT system.
 * ----------
 */
void
pgstat_reset_replslot_counter(const char *name)
{
	PgStat_MsgResetreplslotcounter msg;

	if (pgStatSock == PGINVALID_SOCKET)
		return;

	if (name)
	{
		namestrcpy(&msg.m_slotname, name);
		msg.clearall = false;
	}
	else
		msg.clearall = true;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_RESETREPLSLOTCOUNTER);

	pgstat_send(&msg, sizeof(msg));
}

/* ----------
 * pgstat_send_slru() -
 *
 *		Send SLRU statistics to the collector
 * ----------
 */
void
pgstat_send_slru(void)
{
	/* We assume this initializes to zeroes */
	static const PgStat_MsgSLRU all_zeroes;

	for (int i = 0; i < SLRU_NUM_ELEMENTS; i++)
	{
		/*
		 * This function can be called even if nothing at all has happened. In
		 * this case, avoid sending a completely empty message to the stats
		 * collector.
		 */
		if (memcmp(&SLRUStats[i], &all_zeroes, sizeof(PgStat_MsgSLRU)) == 0)
			continue;

		/* set the SLRU type before each send */
		SLRUStats[i].m_index = i;

		/*
		 * Prepare and send the message
		 */
		pgstat_setheader(&SLRUStats[i].m_hdr, PGSTAT_MTYPE_SLRU);
		pgstat_send(&SLRUStats[i], sizeof(PgStat_MsgSLRU));

		/*
		 * Clear out the statistics buffer, so it can be re-used.
		 */
		MemSet(&SLRUStats[i], 0, sizeof(PgStat_MsgSLRU));
	}
}

/* ----------
 * pgstat_reset_slru_counter() -
 *
 *	Tell the statistics collector to reset a single SLRU counter, or all
 *	SLRU counters (when name is null).
 *
 *	Permission checking for this function is managed through the normal
 *	GRANT system.
 * ----------
 */
void
pgstat_reset_slru_counter(const char *name)
{
	PgStat_MsgResetslrucounter msg;

	if (pgStatSock == PGINVALID_SOCKET)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_RESETSLRUCOUNTER);
	msg.m_index = (name) ? pgstat_slru_index(name) : -1;

	pgstat_send(&msg, sizeof(msg));
}

/*
 * pgstat_slru_index
 *
 * Determine index of entry for a SLRU with a given name. If there's no exact
 * match, returns index of the last "other" entry used for SLRUs defined in
 * external projects.
 */
int
pgstat_slru_index(const char *name)
{
	int			i;

	for (i = 0; i < SLRU_NUM_ELEMENTS; i++)
	{
		if (strcmp(slru_names[i], name) == 0)
			return i;
	}

	/* return index of the last entry (which is the "other" one) */
	return (SLRU_NUM_ELEMENTS - 1);
}

/*
 * pgstat_slru_name
 *
 * Returns SLRU name for an index. The index may be above SLRU_NUM_ELEMENTS,
 * in which case this returns NULL. This allows writing code that does not
 * know the number of entries in advance.
 */
const char *
pgstat_slru_name(int slru_idx)
{
	if (slru_idx < 0 || slru_idx >= SLRU_NUM_ELEMENTS)
		return NULL;

	return slru_names[slru_idx];
}

/*
 * slru_entry
 *
 * Returns pointer to entry with counters for given SLRU (based on the name
 * stored in SlruCtl as lwlock tranche name).
 */
static inline PgStat_MsgSLRU *
slru_entry(int slru_idx)
{
	pgstat_assert_is_up();

	/*
	 * The postmaster should never register any SLRU statistics counts; if it
	 * did, the counts would be duplicated into child processes via fork().
	 */
	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);

	Assert((slru_idx >= 0) && (slru_idx < SLRU_NUM_ELEMENTS));

	return &SLRUStats[slru_idx];
}

/*
 * SLRU statistics count accumulation functions --- called from slru.c
 */

void
pgstat_count_slru_page_zeroed(int slru_idx)
{
	slru_entry(slru_idx)->m_blocks_zeroed += 1;
}

void
pgstat_count_slru_page_hit(int slru_idx)
{
	slru_entry(slru_idx)->m_blocks_hit += 1;
}

void
pgstat_count_slru_page_exists(int slru_idx)
{
	slru_entry(slru_idx)->m_blocks_exists += 1;
}

void
pgstat_count_slru_page_read(int slru_idx)
{
	slru_entry(slru_idx)->m_blocks_read += 1;
}

void
pgstat_count_slru_page_written(int slru_idx)
{
	slru_entry(slru_idx)->m_blocks_written += 1;
}

void
pgstat_count_slru_flush(int slru_idx)
{
	slru_entry(slru_idx)->m_flush += 1;
}

void
pgstat_count_slru_truncate(int slru_idx)
{
	slru_entry(slru_idx)->m_truncate += 1;
}

void
pgstat_wal_initialize(void)
{
	/*
	 * Initialize prevWalUsage with pgWalUsage so that pgstat_send_wal() can
	 * calculate how much pgWalUsage counters are increased by subtracting
	 * prevWalUsage from pgWalUsage.
	 */
	prevWalUsage = pgWalUsage;
}

/* ----------
 * pgstat_send_wal() -
 *
 *	Send WAL statistics to the collector.
 *
 * If 'force' is not set, WAL stats message is only sent if enough time has
 * passed since last one was sent to reach PGSTAT_STAT_INTERVAL.
 * ----------
 */
void
pgstat_send_wal(bool force)
{
	static TimestampTz sendTime = 0;

	/*
	 * This function can be called even if nothing at all has happened. In
	 * this case, avoid sending a completely empty message to the stats
	 * collector.
	 *
	 * Check wal_records counter to determine whether any WAL activity has
	 * happened since last time. Note that other WalUsage counters don't need
	 * to be checked because they are incremented always together with
	 * wal_records counter.
	 *
	 * m_wal_buffers_full also doesn't need to be checked because it's
	 * incremented only when at least one WAL record is generated (i.e.,
	 * wal_records counter is incremented). But for safely, we assert that
	 * m_wal_buffers_full is always zero when no WAL record is generated
	 *
	 * This function can be called by a process like walwriter that normally
	 * generates no WAL records. To determine whether any WAL activity has
	 * happened at that process since the last time, the numbers of WAL writes
	 * and syncs are also checked.
	 */
	if (pgWalUsage.wal_records == prevWalUsage.wal_records &&
		WalStats.m_wal_write == 0 && WalStats.m_wal_sync == 0)
	{
		Assert(WalStats.m_wal_buffers_full == 0);
		return;
	}

	if (!force)
	{
		TimestampTz now = GetCurrentTimestamp();

		/*
		 * Don't send a message unless it's been at least PGSTAT_STAT_INTERVAL
		 * msec since we last sent one to avoid overloading the stats
		 * collector.
		 */
		if (!TimestampDifferenceExceeds(sendTime, now, PGSTAT_STAT_INTERVAL))
			return;
		sendTime = now;
	}

	/*
	 * Set the counters related to generated WAL data if the counters were
	 * updated.
	 */
	if (pgWalUsage.wal_records != prevWalUsage.wal_records)
	{
		WalUsage	walusage;

		/*
		 * Calculate how much WAL usage counters were increased by
		 * subtracting the previous counters from the current ones. Fill the
		 * results in WAL stats message.
		 */
		MemSet(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &prevWalUsage);

		WalStats.m_wal_records = walusage.wal_records;
		WalStats.m_wal_fpi = walusage.wal_fpi;
		WalStats.m_wal_bytes = walusage.wal_bytes;

		/*
		 * Save the current counters for the subsequent calculation of WAL
		 * usage.
		 */
		prevWalUsage = pgWalUsage;
	}

	/*
	 * Prepare and send the message
	 */
	pgstat_setheader(&WalStats.m_hdr, PGSTAT_MTYPE_WAL);
	pgstat_send(&WalStats, sizeof(WalStats));

	/*
	 * Clear out the statistics buffer, so it can be re-used.
	 */
	MemSet(&WalStats, 0, sizeof(WalStats));
}
