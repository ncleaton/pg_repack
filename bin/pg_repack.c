/*
 * pg_repack.c: bin/pg_repack.c
 *
 * Portions Copyright (c) 2008-2011, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2011, Itagaki Takahiro
 * Portions Copyright (c) 2012, The Reorg Development Team
 */

/**
 * @brief Client Modules
 */

const char *PROGRAM_URL		= "http://reorg.github.com/pg_repack";
const char *PROGRAM_EMAIL	= "reorg-general@lists.pgfoundry.org";

#ifdef REPACK_VERSION
/* macro trick to stringify a macro expansion */
#define xstr(s) str(s)
#define str(s) #s
const char *PROGRAM_VERSION = xstr(REPACK_VERSION);
#else
const char *PROGRAM_VERSION = "unknown";
#endif

#include "pgut/pgut-fe.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

/*
 * APPLY_COUNT: Number of applied logs per transaction. Larger values
 * could be faster, but will be long transactions in the REDO phase.
 */
#define APPLY_COUNT		1000

/* The '1/1, -1/0' lock skipped is from the bgwriter on newly promoted
 * servers. See GH ticket #1.
 */
#define SQL_XID_SNAPSHOT \
	"SELECT repack.array_accum(virtualtransaction) FROM pg_locks"\
	" WHERE locktype = 'virtualxid' AND pid <> pg_backend_pid()"\
	" AND (virtualxid, virtualtransaction) <> ('1/1', '-1/0')"

#define SQL_XID_ALIVE \
	"SELECT pid FROM pg_locks WHERE locktype = 'virtualxid'"\
	" AND pid <> pg_backend_pid() AND virtualtransaction = ANY($1)"

/*
 * per-table information
 */
typedef struct repack_table
{
	const char	   *target_name;	/* target: relname */
	Oid				target_oid;		/* target: OID */
	Oid				target_toast;	/* target: toast OID */
	Oid				target_tidx;	/* target: toast index OID */
	Oid				pkid;			/* target: PK OID */
	Oid				ckid;			/* target: CK OID */
	const char	   *create_pktype;	/* CREATE TYPE pk */
	const char	   *create_log;		/* CREATE TABLE log */
	const char	   *create_trigger;	/* CREATE TRIGGER z_repack_trigger */
	const char	   *enable_trigger;	/* ALTER TABLE ENABLE ALWAYS TRIGGER z_repack_trigger */
	const char	   *create_table;	/* CREATE TABLE table AS SELECT */
	const char	   *drop_columns;	/* ALTER TABLE DROP COLUMNs */
	const char	   *delete_log;		/* DELETE FROM log */
	const char	   *lock_table;		/* LOCK TABLE table */
	const char	   *sql_peek;		/* SQL used in flush */
	const char	   *sql_insert;		/* SQL used in flush */
	const char	   *sql_delete;		/* SQL used in flush */
	const char	   *sql_update;		/* SQL used in flush */
	const char	   *sql_pop;		/* SQL used in flush */
} repack_table;

/*
 * per-index information
 */
typedef struct repack_index
{
	Oid				target_oid;		/* target: OID */
	const char	   *create_index;	/* CREATE INDEX */
} repack_index;

static void repack_all_databases(const char *order_by);
static bool repack_one_database(const char *order_by, const char *table, char *errbuf, size_t errsize);
static void repack_one_table(const repack_table *table, const char *order_by);
static void repack_cleanup(bool fatal, void *userdata);

static char *getstr(PGresult *res, int row, int col);
static Oid getoid(PGresult *res, int row, int col);
static void lock_exclusive(const char *relid, const char *lock_query);

#define SQLSTATE_INVALID_SCHEMA_NAME	"3F000"
#define SQLSTATE_QUERY_CANCELED			"57014"

static bool sqlstate_equals(PGresult *res, const char *state)
{
	return strcmp(PQresultErrorField(res, PG_DIAG_SQLSTATE), state) == 0;
}

static bool		analyze = true;
static bool		alldb = false;
static bool		noorder = false;
static char	   *table = NULL;
static char	   *orderby = NULL;
static int		wait_timeout = 60;	/* in seconds */

/* buffer should have at least 11 bytes */
static char *
utoa(unsigned int value, char *buffer)
{
	sprintf(buffer, "%u", value);
	return buffer;
}

static pgut_option options[] =
{
	{ 'b', 'a', "all", &alldb },
	{ 's', 't', "table", &table },
	{ 'b', 'n', "no-order", &noorder },
	{ 's', 'o', "order-by", &orderby },
	{ 'i', 'T', "wait-timeout", &wait_timeout },
	{ 'B', 'Z', "no-analyze", &analyze },
	{ 0 },
};

int
main(int argc, char *argv[])
{
	int		i;

	i = pgut_getopt(argc, argv, options);

	if (i == argc - 1)
		dbname = argv[i];
	else if (i < argc)
		ereport(ERROR,
			(errcode(EINVAL),
			 errmsg("too many arguments")));

	if (noorder)
		orderby = "";

	if (alldb)
	{
		if (table)
			ereport(ERROR,
				(errcode(EINVAL),
				 errmsg("cannot repack a specific table in all databases")));
		repack_all_databases(orderby);
	}
	else
	{
		char	errbuf[256];
		if (!repack_one_database(orderby, table, errbuf, sizeof(errbuf)))
			ereport(ERROR,
				(errcode(ERROR),
				 errmsg("%s", errbuf)));
	}

	return 0;
}

/*
 * Call repack_one_database for each database.
 */
static void
repack_all_databases(const char *orderby)
{
	PGresult   *result;
	int			i;

	dbname = "postgres";
	reconnect(ERROR);
	result = execute("SELECT datname FROM pg_database WHERE datallowconn ORDER BY 1;", 0, NULL);
	disconnect();

	for (i = 0; i < PQntuples(result); i++)
	{
		bool	ret;
		char	errbuf[256];

		dbname = PQgetvalue(result, i, 0);

		if (pgut_log_level >= INFO)
		{
			printf("%s: repack database \"%s\"", PROGRAM_NAME, dbname);
			fflush(stdout);
		}

		ret = repack_one_database(orderby, NULL, errbuf, sizeof(errbuf));

		if (pgut_log_level >= INFO)
		{
			if (ret)
				printf("\n");
			else
				printf(" ... skipped: %s\n", errbuf);
			fflush(stdout);
		}
	}

	PQclear(result);
}

/* result is not copied */
static char *
getstr(PGresult *res, int row, int col)
{
	if (PQgetisnull(res, row, col))
		return NULL;
	else
		return PQgetvalue(res, row, col);
}

static Oid
getoid(PGresult *res, int row, int col)
{
	if (PQgetisnull(res, row, col))
		return InvalidOid;
	else
		return (Oid)strtoul(PQgetvalue(res, row, col), NULL, 10);
}

/*
 * Call repack_one_table for the target table or each table in a database.
 */
static bool
repack_one_database(const char *orderby, const char *table, char *errbuf, size_t errsize)
{
	bool			ret = false;
	PGresult	   *res;
	int				i;
	int				num;
	StringInfoData	sql;

	initStringInfo(&sql);

	reconnect(ERROR);

	/* Query the extension version. Exit if no match */
	res = execute_elevel("select repack.version(), repack.version_sql()",
		0, NULL, DEBUG2);
	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		const char	   *libver;
		char			buf[64];

		/* the string is something like "pg_repack 1.1.7" */
		snprintf(buf, sizeof(buf), "%s %s", PROGRAM_NAME, PROGRAM_VERSION);

		/* check the version of the C library */
		libver = getstr(res, 0, 0);
		if (0 != strcmp(buf, libver))
		{
			if (errbuf)
				snprintf(errbuf, errsize,
					"program '%s' does not match database library '%s'",
					buf, libver);
			goto cleanup;
		}

		/* check the version of the SQL extension */
		libver = getstr(res, 0, 1);
		if (0 != strcmp(buf, libver))
		{
			if (errbuf)
				snprintf(errbuf, errsize,
					"extension '%s' required, found extension '%s'",
					buf, libver);
			goto cleanup;
		}
	}
	else
	{
		if (sqlstate_equals(res, SQLSTATE_INVALID_SCHEMA_NAME))
		{
			/* Schema repack does not exist. Skip the database. */
			if (errbuf)
				snprintf(errbuf, errsize,
					"%s is not installed in the database", PROGRAM_NAME);
		}
		else
		{
			/* Return the error message otherwise */
			if (errbuf)
				snprintf(errbuf, errsize, "%s", PQerrorMessage(connection));
		}
		goto cleanup;
	}

	/* Disable statement timeout. */
	command("SET statement_timeout = 0", 0, NULL);

	/* Restrict search_path to system catalog. */
	command("SET search_path = pg_catalog, pg_temp, public", 0, NULL);

	/* To avoid annoying "create implicit ..." messages. */
	command("SET client_min_messages = warning", 0, NULL);

	/* acquire target tables */
	appendStringInfoString(&sql, "SELECT * FROM repack.tables WHERE ");
	if (table)
	{
		appendStringInfoString(&sql, "relid = $1::regclass");
		res = execute_elevel(sql.data, 1, &table, DEBUG2);
	}
	else
	{
		appendStringInfoString(&sql, "pkid IS NOT NULL");
		if (!orderby)
			appendStringInfoString(&sql, " AND ckid IS NOT NULL");
		res = execute_elevel(sql.data, 0, NULL, DEBUG2);
	}

	/* on error skip the database */
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		if (sqlstate_equals(res, SQLSTATE_INVALID_SCHEMA_NAME))
		{
			/* Schema repack does not exist. Skip the database. */
			if (errbuf)
				snprintf(errbuf, errsize,
					"%s is not installed in the database", PROGRAM_NAME);
		}
		else
		{
			/* Return the error message otherwise */
			if (errbuf)
				snprintf(errbuf, errsize, "%s", PQerrorMessage(connection));
		}
		ret = false;
		goto cleanup;
	}

	num = PQntuples(res);

	for (i = 0; i < num; i++)
	{
		repack_table	table;
		const char *create_table;
		const char *ckey;
		int			c = 0;

		table.target_name = getstr(res, i, c++);
		table.target_oid = getoid(res, i, c++);
		table.target_toast = getoid(res, i, c++);
		table.target_tidx = getoid(res, i, c++);
		table.pkid = getoid(res, i, c++);
		table.ckid = getoid(res, i, c++);

		if (table.pkid == 0)
			ereport(ERROR,
				(errcode(E_PG_COMMAND),
				 errmsg("relation \"%s\" must have a primary key or not-null unique keys", table.target_name)));

		table.create_pktype = getstr(res, i, c++);
		table.create_log = getstr(res, i, c++);
		table.create_trigger = getstr(res, i, c++);
		table.enable_trigger = getstr(res, i, c++);

		create_table = getstr(res, i, c++);
		table.drop_columns = getstr(res, i, c++);
		table.delete_log = getstr(res, i, c++);
		table.lock_table = getstr(res, i, c++);
		ckey = getstr(res, i, c++);

		resetStringInfo(&sql);
		if (!orderby)
		{
			/* CLUSTER mode */
			if (ckey == NULL)
				ereport(ERROR,
					(errcode(E_PG_COMMAND),
					 errmsg("relation \"%s\" has no cluster key", table.target_name)));
			appendStringInfo(&sql, "%s ORDER BY %s", create_table, ckey);
            table.create_table = sql.data;
		}
		else if (!orderby[0])
		{
			/* VACUUM FULL mode */
            table.create_table = create_table;
		}
		else
		{
			/* User specified ORDER BY */
			appendStringInfo(&sql, "%s ORDER BY %s", create_table, orderby);
            table.create_table = sql.data;
		}

		table.sql_peek = getstr(res, i, c++);
		table.sql_insert = getstr(res, i, c++);
		table.sql_delete = getstr(res, i, c++);
		table.sql_update = getstr(res, i, c++);
		table.sql_pop = getstr(res, i, c++);

		repack_one_table(&table, orderby);
	}
	ret = true;

cleanup:
	PQclear(res);
	disconnect();
	termStringInfo(&sql);
	return ret;
}

static int
apply_log(const repack_table *table, int count)
{
	int			result;
	PGresult   *res;
	const char *params[6];
	char		buffer[12];

	params[0] = table->sql_peek;
	params[1] = table->sql_insert;
	params[2] = table->sql_delete;
	params[3] = table->sql_update;
	params[4] = table->sql_pop;
	params[5] = utoa(count, buffer);

	res = execute("SELECT repack.repack_apply($1, $2, $3, $4, $5, $6)",
		6, params);
	result = atoi(PQgetvalue(res, 0, 0));
	PQclear(res);

	return result;
}

/*
 * Re-organize one table.
 */
static void
repack_one_table(const repack_table *table, const char *orderby)
{
	PGresult	   *res;
	const char	   *params[1];
	int				num;
	int				i;
	int				num_waiting = 0;
	char		   *vxid;
	char			buffer[12];
	StringInfoData	sql;

	initStringInfo(&sql);

	elog(DEBUG2, "---- repack_one_table ----");
	elog(DEBUG2, "target_name    : %s", table->target_name);
	elog(DEBUG2, "target_oid     : %u", table->target_oid);
	elog(DEBUG2, "target_toast   : %u", table->target_toast);
	elog(DEBUG2, "target_tidx    : %u", table->target_tidx);
	elog(DEBUG2, "pkid           : %u", table->pkid);
	elog(DEBUG2, "ckid           : %u", table->ckid);
	elog(DEBUG2, "create_pktype  : %s", table->create_pktype);
	elog(DEBUG2, "create_log     : %s", table->create_log);
	elog(DEBUG2, "create_trigger : %s", table->create_trigger);
	elog(DEBUG2, "enable_trigger : %s", table->enable_trigger);
	elog(DEBUG2, "create_table   : %s", table->create_table);
	elog(DEBUG2, "drop_columns   : %s", table->drop_columns ? table->drop_columns : "(skipped)");
	elog(DEBUG2, "delete_log     : %s", table->delete_log);
	elog(DEBUG2, "lock_table     : %s", table->lock_table);
	elog(DEBUG2, "sql_peek       : %s", table->sql_peek);
	elog(DEBUG2, "sql_insert     : %s", table->sql_insert);
	elog(DEBUG2, "sql_delete     : %s", table->sql_delete);
	elog(DEBUG2, "sql_update     : %s", table->sql_update);
	elog(DEBUG2, "sql_pop        : %s", table->sql_pop);

	/*
	 * 1. Setup workspaces and a trigger.
	 */
	elog(DEBUG2, "---- setup ----");
	lock_exclusive(utoa(table->target_oid, buffer), table->lock_table);

	/*
	 * Check z_repack_trigger is the trigger executed at last so that
	 * other before triggers cannot modify triggered tuples.
	 */
	params[0] = utoa(table->target_oid, buffer);

	res = execute("SELECT repack.conflicted_triggers($1)", 1, params);
	if (PQntuples(res) > 0)
		ereport(ERROR,
			(errcode(E_PG_COMMAND),
			 errmsg("trigger %s conflicted for %s",
					PQgetvalue(res, 0, 0), table->target_name)));
	PQclear(res);

	command(table->create_pktype, 0, NULL);
	command(table->create_log, 0, NULL);
	command(table->create_trigger, 0, NULL);
	command(table->enable_trigger, 0, NULL);
	printfStringInfo(&sql, "SELECT repack.disable_autovacuum('repack.log_%u')", table->target_oid);
	command(sql.data, 0, NULL);
	command("COMMIT", 0, NULL);

	/*
	 * Register the table to be dropped on error. We use pktype as
	 * an advisory lock. The registration should be done after
	 * the first command succeeds.
	 */
	pgut_atexit_push(&repack_cleanup, (void *) table);

	/*
	 * 2. Copy tuples into temp table.
	 */
	elog(DEBUG2, "---- copy tuples ----");

	/* Must use SERIALIZABLE (or at least not READ COMMITTED) to avoid race
	 * condition between the create_table statement and rows subsequently
	 * being added to the log.
	 */
	command("BEGIN ISOLATION LEVEL SERIALIZABLE", 0, NULL);
	/* SET work_mem = maintenance_work_mem */
	command("SELECT set_config('work_mem', current_setting('maintenance_work_mem'), true)", 0, NULL);
	if (orderby && !orderby[0])
		command("SET LOCAL synchronize_seqscans = off", 0, NULL);
	res = execute(SQL_XID_SNAPSHOT, 0, NULL);
	vxid = strdup(PQgetvalue(res, 0, 0));
	PQclear(res);

	/* Delete any existing entries in the log table now, since we have not
	 * yet run the CREATE TABLE ... AS SELECT, which will take in all existing
	 * rows from the target table; if we also included prior rows from the
	 * log we could wind up with duplicates.
	 */
	command(table->delete_log, 0, NULL);
	command(table->create_table, 0, NULL);
	printfStringInfo(&sql, "SELECT repack.disable_autovacuum('repack.table_%u')", table->target_oid);
	if (table->drop_columns)
		command(table->drop_columns, 0, NULL);
	command(sql.data, 0, NULL);
	command("COMMIT", 0, NULL);

	/*
	 * 3. Create indexes on temp table.
	 */
	elog(DEBUG2, "---- create indexes ----");

	params[0] = utoa(table->target_oid, buffer);
	res = execute("SELECT indexrelid,"
		" repack.repack_indexdef(indexrelid, indrelid),"
		" indisvalid,"
		" pg_get_indexdef(indexrelid)"
		" FROM pg_index WHERE indrelid = $1", 1, params);

	num = PQntuples(res);
	for (i = 0; i < num; i++)
	{
		repack_index	index;
		int			c = 0;
		const char *isvalid;
		const char *indexdef;

		index.target_oid = getoid(res, i, c++);
		index.create_index = getstr(res, i, c++);
		isvalid = getstr(res, i, c++);
		indexdef = getstr(res, i, c++);

		if (isvalid && isvalid[0] == 'f') {
			elog(WARNING, "skipping invalid index: %s", indexdef);
			continue;
		}

		elog(DEBUG2, "[%d]", i);
		elog(DEBUG2, "target_oid   : %u", index.target_oid);
		elog(DEBUG2, "create_index : %s", index.create_index);

		/*
		 * NOTE: If we want to create multiple indexes in parallel,
		 * we need to call create_index in multiple connections.
		 */
		command(index.create_index, 0, NULL);
	}
	PQclear(res);

	/*
	 * 4. Apply log to temp table until no tuples are left in the log
	 * and all of the old transactions are finished.
	 */
	for (;;)
	{
		num = apply_log(table, APPLY_COUNT);
		if (num > 0)
			continue;	/* there might be still some tuples, repeat. */

		/* old transactions still alive ? */
		params[0] = vxid;
		res = execute(SQL_XID_ALIVE, 1, params);
		num = PQntuples(res);

		if (num > 0)
		{
			/* Wait for old transactions.
			 * Only display the message below when the number of
			 * transactions we are waiting on changes (presumably,
			 * num_waiting should only go down), so as not to
			 * be too noisy.
			 */
			if (num != num_waiting)
			{
				elog(NOTICE, "Waiting for %d transactions to finish. First PID: %s", num, PQgetvalue(res, 0, 0));
				num_waiting = num;
			}

			PQclear(res);
			sleep(1);
			continue;
		}
		else
		{
			/* All old transactions are finished;
			 * go to next step. */
			PQclear(res);
			break;
		}
	}

	/*
	 * 5. Swap.
	 */
	elog(DEBUG2, "---- swap ----");
	lock_exclusive(utoa(table->target_oid, buffer), table->lock_table);
	apply_log(table, 0);
	params[0] = utoa(table->target_oid, buffer);
	command("SELECT repack.repack_swap($1)", 1, params);
	command("COMMIT", 0, NULL);

	/*
	 * 6. Drop.
	 */
	elog(DEBUG2, "---- drop ----");

	command("BEGIN ISOLATION LEVEL READ COMMITTED", 0, NULL);
	params[0] = utoa(table->target_oid, buffer);
	command("SELECT repack.repack_drop($1)", 1, params);
	command("COMMIT", 0, NULL);

	pgut_atexit_pop(&repack_cleanup, (void *) table);
	free(vxid);

	/*
	 * 7. Analyze.
	 * Note that cleanup hook has been already uninstalled here because analyze
	 * is not an important operation; No clean up even if failed.
	 */
	if (analyze)
	{
		elog(DEBUG2, "---- analyze ----");

		command("BEGIN ISOLATION LEVEL READ COMMITTED", 0, NULL);
		printfStringInfo(&sql, "ANALYZE %s", table->target_name);
		command(sql.data, 0, NULL);
		command("COMMIT", 0, NULL);
	}

	termStringInfo(&sql);
}

/*
 * Try acquire a table lock but avoid long time locks when conflict.
 */
static void
lock_exclusive(const char *relid, const char *lock_query)
{
	time_t		start = time(NULL);
	int			i;
	
	for (i = 1; ; i++)
	{
		time_t		duration;
		char		sql[1024];
		PGresult   *res;
		int			wait_msec;

		command("BEGIN ISOLATION LEVEL READ COMMITTED", 0, NULL);

		duration = time(NULL) - start;
		if (duration > wait_timeout)
		{
			const char *cancel_query;
			if (PQserverVersion(connection) >= 80400 &&
				duration > wait_timeout * 2)
			{
				elog(WARNING, "terminating conflicted backends");
				cancel_query =
					"SELECT pg_terminate_backend(pid) FROM pg_locks"
					" WHERE locktype = 'relation'"
					"   AND relation = $1 AND pid <> pg_backend_pid()";
			}
			else
			{
				elog(WARNING, "canceling conflicted backends");
				cancel_query =
					"SELECT pg_cancel_backend(pid) FROM pg_locks"
					" WHERE locktype = 'relation'"
					"   AND relation = $1 AND pid <> pg_backend_pid()";
			}

			command(cancel_query, 1, &relid);
		}

		/* wait for a while to lock the table. */
		wait_msec = Min(1000, i * 100);
		snprintf(sql, lengthof(sql), "SET LOCAL statement_timeout = %d", wait_msec);
		command(sql, 0, NULL);

		res = execute_elevel(lock_query, 0, NULL, DEBUG2);
		if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			PQclear(res);
			break;
		}
		else if (sqlstate_equals(res, SQLSTATE_QUERY_CANCELED))
		{
			/* retry if lock conflicted */
			PQclear(res);
			command("ROLLBACK", 0, NULL);
			continue;
		}
		else
		{
			/* exit otherwise */
			printf("%s", PQerrorMessage(connection));
			PQclear(res);
			exit(1);
		}
	}

	command("RESET statement_timeout", 0, NULL);
}

/*
 * The userdata pointing a table being re-organized. We need to cleanup temp
 * objects before the program exits.
 */
static void
repack_cleanup(bool fatal, void *userdata)
{
	const repack_table *table = (const repack_table *) userdata;

	if (fatal)
	{
		fprintf(stderr, "!!!FATAL ERROR!!! Please refer to the manual.\n\n");
	}
	else
	{
		char		buffer[12];
		const char *params[1];

		/* Rollback current transaction */
		if (connection)
			command("ROLLBACK", 0, NULL);

		/* Try reconnection if not available. */
		if (PQstatus(connection) != CONNECTION_OK)
			reconnect(ERROR);

		/* do cleanup */
		params[0] = utoa(table->target_oid, buffer);
		command("SELECT repack.repack_drop($1)", 1, params);
	}
}

void
pgut_help(bool details)
{
	printf("%s re-organizes a PostgreSQL database.\n\n", PROGRAM_NAME);
	printf("Usage:\n");
	printf("  %s [OPTION]... [DBNAME]\n", PROGRAM_NAME);

	if (!details)
		return;

	printf("Options:\n");
	printf("  -a, --all                 repack all databases\n");
	printf("  -n, --no-order            do vacuum full instead of cluster\n");
	printf("  -o, --order-by=COLUMNS    order by columns instead of cluster keys\n");
	printf("  -t, --table=TABLE         repack specific table only\n");
	printf("  -T, --wait-timeout=SECS   timeout to cancel other backends on conflict\n");
	printf("  -Z, --no-analyze          don't analyze at end\n");
}
