/*
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief SMSdb
 *
 * \author Max von Buelow <max@m9x.de>
 * \author Mark Spencer <markster@digium.com>
 *
 * The original code is from the astdb prat of the Asterisk project.
 */

#include <dirent.h>
#include <signal.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <asterisk.h>

#include <asterisk/app.h>
#include <asterisk/utils.h>

#include "smsdb.h"

#include "chan_quectel.h"

static const size_t DBKEY_DEF_LEN = 32;
static const size_t DBKEY_MAX_LEN = 256;

AST_MUTEX_DEFINE_STATIC(dblock);
static sqlite3* smsdb;

static void smsdb_stmt_begin(sqlite3_stmt* stmt)
{
    if (!stmt) {
        return;
    }
}

static void smsdb_stmt_end(sqlite3_stmt* stmt)
{
    if (!stmt) {
        return;
    }

    const int res = sqlite3_reset(stmt);
    if (res != SQLITE_OK) {
        ast_log(LOG_ERROR, "Fail to reset statement: %s\n", sqlite3_errstr(res));
    }
}

#define SCOPED_STMT(s) SCOPED_LOCK(s, s##_stmt, smsdb_stmt_begin, smsdb_stmt_end)

#define DEFINE_SQL_STATEMENT(s, sql)      \
    static sqlite3_stmt* s##_stmt = NULL; \
    static const char s##_sql[]   = sql;

DEFINE_SQL_STATEMENT(get_full_message, "SELECT message FROM incoming WHERE key = ? ORDER BY seqorder")
DEFINE_SQL_STATEMENT(put_message,
                     "INSERT OR REPLACE INTO incoming (key, seqorder, expiration, message) VALUES (?, ?, "
                     "datetime(julianday(CURRENT_TIMESTAMP) + ? / 86400.0), ?)")
DEFINE_SQL_STATEMENT(clear_messages, "DELETE FROM incoming WHERE key = ?")
DEFINE_SQL_STATEMENT(purge_messages, "DELETE FROM incoming WHERE expiration < CURRENT_TIMESTAMP")
DEFINE_SQL_STATEMENT(get_cnt, "SELECT COUNT(seqorder) FROM incoming WHERE key = ?")
DEFINE_SQL_STATEMENT(create_incoming,
                     "CREATE TABLE IF NOT EXISTS incoming (key VARCHAR(256), seqorder INTEGER, expiration TIMESTAMP "
                     "DEFAULT CURRENT_TIMESTAMP, message VARCHAR(256), PRIMARY KEY(key, seqorder))")
DEFINE_SQL_STATEMENT(create_index, "CREATE INDEX IF NOT EXISTS incoming_key ON incoming(key)")
DEFINE_SQL_STATEMENT(create_outgoingref,
                     "CREATE TABLE IF NOT EXISTS outgoing_ref (key VARCHAR(256), refid INTEGER, PRIMARY KEY(key))")  // key:
                                                                                                                     // IMSI/DEST_ADDR
DEFINE_SQL_STATEMENT(create_outgoingmsg,
                     "CREATE TABLE IF NOT EXISTS outgoing_msg (uid INTEGER PRIMARY KEY AUTOINCREMENT, dev "
                     "VARCHAR(256), dst VARCHAR(255), cnt INTEGER, expiration TIMESTAMP, srr BOOLEAN)")
DEFINE_SQL_STATEMENT(create_outgoingpart,
                     "CREATE TABLE IF NOT EXISTS outgoing_part (key VARCHAR(256), msg INTEGER, status INTEGER, PRIMARY "
                     "KEY(key))")  // key: IMSI/DEST_ADDR/MR
DEFINE_SQL_STATEMENT(create_outgoingmsg_index, "CREATE INDEX IF NOT EXISTS outgoing_part_msg ON outgoing_part(msg)")
DEFINE_SQL_STATEMENT(ins_outgoingref,
                     "INSERT INTO outgoing_ref (refid, key) VALUES (?, ?)")  // This have to be the same order as set_outgoingref_stmt
DEFINE_SQL_STATEMENT(set_outgoingref, "UPDATE outgoing_ref SET refid = ? WHERE key = ?")
DEFINE_SQL_STATEMENT(get_outgoingref, "SELECT refid FROM outgoing_ref WHERE key = ?")
DEFINE_SQL_STATEMENT(put_outgoingmsg,
                     "INSERT INTO outgoing_msg (dev, dst, cnt, expiration, srr) VALUES (?, ?, ?, "
                     "datetime(julianday(CURRENT_TIMESTAMP) + ? / 86400.0), ?)")
DEFINE_SQL_STATEMENT(put_outgoingpart, "INSERT INTO outgoing_part (key, msg, status) VALUES (?, ?, NULL)")
DEFINE_SQL_STATEMENT(del_outgoingmsg, "DELETE FROM outgoing_msg WHERE uid = ?")
DEFINE_SQL_STATEMENT(del_outgoingpart, "DELETE FROM outgoing_part WHERE msg = ?")
DEFINE_SQL_STATEMENT(get_outgoingmsg, "SELECT dev, dst, srr FROM outgoing_msg WHERE uid = ?")
DEFINE_SQL_STATEMENT(set_outgoingpart, "UPDATE outgoing_part SET status = ? WHERE rowid = ?")
DEFINE_SQL_STATEMENT(get_outgoingpart, "SELECT rowid, msg FROM outgoing_part WHERE key = ?")
DEFINE_SQL_STATEMENT(cnt_outgoingpart,
                     "SELECT m.cnt, (SELECT COUNT(p.rowid) FROM outgoing_part p WHERE p.msg = m.rowid AND (p.status & 64 != 0 OR "
                     "p.status & 32 = 0)) FROM outgoing_msg m WHERE m.rowid = ?")  // count all failed and completed messages; don't
                                                                                   // count messages without delivery notification and
                                                                                   // temporary failed ones
DEFINE_SQL_STATEMENT(cnt_all_outgoingpart,
                     "SELECT m.cnt, (SELECT COUNT(p.rowid) FROM outgoing_part p WHERE p.msg = m.uid) FROM outgoing_msg "
                     "m WHERE m.uid = ?")
DEFINE_SQL_STATEMENT(get_dst, "SELECT dst FROM outgoing_msg WHERE uid = ?")
DEFINE_SQL_STATEMENT(get_all_status, "SELECT status FROM outgoing_part WHERE msg = ? ORDER BY rowid")
DEFINE_SQL_STATEMENT(get_expired,
                     "SELECT uid, dst FROM outgoing_msg WHERE expiration < CURRENT_TIMESTAMP LIMIT 1")  // only fetch one
                                                                                                        // expired row to
                                                                                                        // balance the load of
                                                                                                        // each transaction

static int init_stmt(sqlite3_stmt** stmt, const char* sql, size_t len)
{
    SCOPED_MUTEX(dblock_lock, &dblock);

    if (sqlite3_prepare(smsdb, sql, len, stmt, NULL) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't prepare statement '%s': %s\n", sql, sqlite3_errmsg(smsdb));
        return -1;
    }

    return 0;
}

#define INIT_STMT(s) init_stmt(&s##_stmt, s##_sql, sizeof(s##_sql))

/*! \internal
 * \brief Clean up the prepared SQLite3 statement
 * \note dblock should already be locked prior to calling this method
 */
static int clean_stmt(sqlite3_stmt** stmt, const char* sql)
{
    if (sqlite3_finalize(*stmt) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't finalize statement '%s': %s\n", sql, sqlite3_errmsg(smsdb));
        *stmt = NULL;
        return -1;
    }
    *stmt = NULL;
    return 0;
}

#define CLEAN_STMT(s) clean_stmt(&s##_stmt, s##_sql)

/*! \internal
 * \brief Clean up all prepared SQLite3 statements
 * \note dblock should already be locked prior to calling this method
 */
static void clean_statements(void)
{
    CLEAN_STMT(get_full_message);
    CLEAN_STMT(put_message);
    CLEAN_STMT(clear_messages);
    CLEAN_STMT(purge_messages);
    CLEAN_STMT(get_cnt);
    CLEAN_STMT(create_incoming);
    CLEAN_STMT(create_index);
    CLEAN_STMT(create_outgoingref);
    CLEAN_STMT(create_outgoingmsg);
    CLEAN_STMT(create_outgoingpart);
    CLEAN_STMT(create_outgoingmsg_index);
    CLEAN_STMT(ins_outgoingref);
    CLEAN_STMT(set_outgoingref);
    CLEAN_STMT(get_outgoingref);
    CLEAN_STMT(put_outgoingmsg);
    CLEAN_STMT(put_outgoingpart);
    CLEAN_STMT(del_outgoingmsg);
    CLEAN_STMT(del_outgoingpart);
    CLEAN_STMT(get_outgoingmsg);
    CLEAN_STMT(set_outgoingpart);
    CLEAN_STMT(get_outgoingpart);
    CLEAN_STMT(cnt_outgoingpart);
    CLEAN_STMT(cnt_all_outgoingpart);
    CLEAN_STMT(get_dst);
    CLEAN_STMT(get_all_status);
    CLEAN_STMT(get_expired);
}

static int init_statements(void)
{
    /* Don't initialize create_smsdb_statement here as the smsdb table needs to exist
     * brefore these statements can be initialized */
    return INIT_STMT(get_full_message) || INIT_STMT(put_message) || INIT_STMT(clear_messages) || INIT_STMT(purge_messages) || INIT_STMT(get_cnt) ||
           INIT_STMT(ins_outgoingref) || INIT_STMT(set_outgoingref) || INIT_STMT(get_outgoingref) || INIT_STMT(put_outgoingmsg) ||
           INIT_STMT(put_outgoingpart) || INIT_STMT(del_outgoingmsg) || INIT_STMT(del_outgoingpart) || INIT_STMT(get_outgoingmsg) ||
           INIT_STMT(get_outgoingpart) || INIT_STMT(set_outgoingpart) || INIT_STMT(cnt_outgoingpart) || INIT_STMT(cnt_all_outgoingpart) || INIT_STMT(get_dst) ||
           INIT_STMT(get_all_status) || INIT_STMT(get_expired);
}

static int db_create_smsdb(void)
{
    int res = 0;

    if (!create_incoming_stmt) {
        INIT_STMT(create_incoming);
    }

    {
        SCOPED_MUTEX(dblock_lock, &dblock);
        SCOPED_STMT(create_incoming);
        if (sqlite3_step(create_incoming) != SQLITE_DONE) {
            ast_log(LOG_WARNING, "Couldn't create smsdb table: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        }
    }

    if (!create_index_stmt) {
        INIT_STMT(create_index);
    }

    {
        SCOPED_MUTEX(dblock_lock, &dblock);
        SCOPED_STMT(create_index);
        if (sqlite3_step(create_index) != SQLITE_DONE) {
            ast_log(LOG_WARNING, "Couldn't create smsdb index: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        }
    }

    if (!create_outgoingref_stmt) {
        INIT_STMT(create_outgoingref);
    }

    {
        SCOPED_MUTEX(dblock_lock, &dblock);
        SCOPED_STMT(create_outgoingref);
        if (sqlite3_step(create_outgoingref) != SQLITE_DONE) {
            ast_log(LOG_WARNING, "Couldn't create smsdb outgoing table: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        }
    }

    if (!create_outgoingmsg_stmt) {
        INIT_STMT(create_outgoingmsg);
    }

    {
        SCOPED_MUTEX(dblock_lock, &dblock);
        SCOPED_STMT(create_outgoingmsg);
        if (sqlite3_step(create_outgoingmsg) != SQLITE_DONE) {
            ast_log(LOG_WARNING, "Couldn't create smsdb outgoing table: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        }
    }

    if (!create_outgoingpart_stmt) {
        INIT_STMT(create_outgoingpart);
    }

    {
        SCOPED_MUTEX(dblock_lock, &dblock);
        SCOPED_STMT(create_outgoingpart);
        if (sqlite3_step(create_outgoingpart) != SQLITE_DONE) {
            ast_log(LOG_WARNING, "Couldn't create smsdb outgoing table: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        }
    }

    if (!create_outgoingmsg_index_stmt) {
        INIT_STMT(create_outgoingmsg_index);
    }

    {
        SCOPED_MUTEX(dblock_lock, &dblock);
        SCOPED_STMT(create_outgoingmsg_index);
        if (sqlite3_step(create_outgoingmsg_index) != SQLITE_DONE) {
            ast_log(LOG_WARNING, "Couldn't create smsdb outgoing index: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        }
    }

    return res;
}

static int db_name_in_memory(const char* db)
{
    static const char SQLITE_IN_MEMORY_SPECIAL_NAME[] = ":memory:";

    if (db == NULL) {
        return 0;
    }
    return !strncmp(db, SQLITE_IN_MEMORY_SPECIAL_NAME, STRLEN(SQLITE_IN_MEMORY_SPECIAL_NAME));
}

static int db_name_temporary(const char* db)
{
    static const char SQLITE_TMP_SPECIAL_NAME[] = ":temporary:";

    if (db == NULL) {
        return 0;
    }
    return !strncmp(db, SQLITE_TMP_SPECIAL_NAME, STRLEN(SQLITE_TMP_SPECIAL_NAME));
}

static int db_open(void)
{
    static const char SQLITE_DB_EXT[] = ".sqlite3";

    char* dbname;
    if (db_name_in_memory(CONF_GLOBAL(sms_db))) {
        dbname = CONF_GLOBAL(sms_db);
    } else if (db_name_temporary(CONF_GLOBAL(sms_db))) {
        dbname = "";
    } else {
        if (!(dbname = ast_alloca(strlen(CONF_GLOBAL(sms_db)) + STRLEN(SQLITE_DB_EXT) + 1u))) {
            return -1;
        }
        strcpy(dbname, CONF_GLOBAL(sms_db));
        strcat(dbname, SQLITE_DB_EXT);
    }

    SCOPED_MUTEX(dblock_lock, &dblock);
    if (sqlite3_open(dbname, &smsdb) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Unable to open Asterisk database '%s': %s\n", dbname, sqlite3_errmsg(smsdb));
        sqlite3_close(smsdb);
        return -1;
    }

    return 0;
}

static int db_init()
{
    if (smsdb) {
        return 0;
    }

    if (db_open() || db_create_smsdb() || init_statements()) {
        return -1;
    }

    return 0;
}

/* We purposely don't lock around the sqlite3 call because the transaction
 * calls will be called with the database lock held. For any other use, make
 * sure to take the dblock yourself. */
static int db_execute_sql(const char* sql, int (*callback)(void*, int, char**, char**), void* arg)
{
    char* errmsg = NULL;
    int res      = 0;

    if (sqlite3_exec(smsdb, sql, callback, arg, &errmsg) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Error executing SQL (%s): %s\n", sql, errmsg);
        sqlite3_free(errmsg);
        res = -1;
    }

    return res;
}

static void smsdb_begin_transaction(ast_mutex_t* mutex)
{
    ast_mutex_lock(mutex);
    const int res = db_execute_sql("BEGIN TRANSACTION", NULL, NULL);
    if (res != SQLITE_OK) {
        ast_log(LOG_ERROR, "Fail to begin transaction: %d\n", res);
    }
}

static void smsdb_commit_transaction(ast_mutex_t* mutex)
{
    const int res = db_execute_sql("COMMIT", NULL, NULL);
    ast_mutex_unlock(mutex);
    if (res != SQLITE_OK) {
        ast_log(LOG_ERROR, "Fail to commit transaction: %d\n", res);
    }
}

#define SCOPED_TRANSACTION(varname) SCOPED_LOCK(varname, &dblock, smsdb_begin_transaction, smsdb_commit_transaction)

/*!
 * \brief Adds a message part into the DB and returns the whole message into 'out' when the message is complete.
 * \param id -- Some ID for the device or so, e.g. the IMSI
 * \param addr -- The sender address
 * \param ref -- The reference ID
 * \param parts -- The total number of messages
 * \param order -- The current message number
 * \param msg -- The current message part
 * \param out -- Output: Only written if parts == cnt
 * \retval <=0 Error
 * \retval >0 Current number of messages in the DB
 */
int smsdb_put(const char* id, const char* addr, int ref, int parts, int order, const char* msg, char* out)
{
    int res = 0;
    int ttl = CONF_GLOBAL(csms_ttl);

    RAII_VAR(struct ast_str*, fullkey, ast_str_create(DBKEY_DEF_LEN), ast_free);
    const int fullkey_len = ast_str_set(&fullkey, DBKEY_MAX_LEN, "%s/%s/%d/%d", id, addr, ref, parts);
    if (fullkey_len < 0) {
        ast_log(LOG_ERROR, "Fail to create key\n");
        return -1;
    }

    SCOPED_TRANSACTION(dbtrans);

    {
        SCOPED_STMT(put_message);
        if (sqlite3_bind_text(put_message, 1, ast_str_buffer(fullkey), ast_str_strlen(fullkey), SQLITE_STATIC) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_bind_int(put_message, 2, order) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind order to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_bind_int(put_message, 3, ttl) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind TTL to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_bind_text(put_message, 4, msg, -1, SQLITE_STATIC) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind msg to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(put_message) != SQLITE_DONE) {
            ast_log(LOG_WARNING, "Couldn't execute statement: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        }
    }

    {
        SCOPED_STMT(get_cnt);
        if (sqlite3_bind_text(get_cnt, 1, ast_str_buffer(fullkey), ast_str_strlen(fullkey), SQLITE_STATIC) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(get_cnt) != SQLITE_ROW) {
            ast_debug(1, "Unable to find key '%s'\n", ast_str_buffer(fullkey));
            res = -1;
        }
        res = sqlite3_column_int(get_cnt, 0);
    }

    if (res > 0 && order == parts) {
        {
            SCOPED_STMT(get_full_message);
            if (sqlite3_bind_text(get_full_message, 1, ast_str_buffer(fullkey), ast_str_strlen(fullkey), SQLITE_STATIC) != SQLITE_OK) {
                ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
                res = -1;
            } else {
                while (sqlite3_step(get_full_message) == SQLITE_ROW) {
                    const char* part = (const char*)sqlite3_column_text(get_full_message, 0);
                    if (!part) {
                        ast_log(LOG_WARNING, "Couldn't get value\n");
                        res = -1;
                        break;
                    }
                    const int partlen = sqlite3_column_bytes(get_full_message, 0);
                    out               = stpncpy(out, part, partlen);
                }
            }
            out[0] = '\0';
        }

        if (res >= 0) {
            SCOPED_STMT(clear_messages);
            if (sqlite3_bind_text(clear_messages, 1, ast_str_buffer(fullkey), ast_str_strlen(fullkey), SQLITE_STATIC) != SQLITE_OK) {
                ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
                res = -1;
            } else if (sqlite3_step(clear_messages) != SQLITE_DONE) {
                ast_debug(1, "Unable to find key '%s'; Ignoring\n", ast_str_buffer(fullkey));
            }
        }
    }

    return res;
}

int smsdb_get_refid(const char* id, const char* addr)
{
    int res = 0;

    SCOPED_TRANSACTION(dbtrans);
    RAII_VAR(struct ast_str*, fullkey, ast_str_create(DBKEY_DEF_LEN), ast_free);

    const int fullkey_len = ast_str_set(&fullkey, DBKEY_MAX_LEN, "%s/%s", id, addr);
    if (fullkey_len < 0) {
        ast_log(LOG_ERROR, "Fail to create key\n");
        return -1;
    }

    int use_insert = 0;

    {
        SCOPED_STMT(get_outgoingref);
        if (sqlite3_bind_text(get_outgoingref, 1, ast_str_buffer(fullkey), ast_str_strlen(fullkey), SQLITE_STATIC) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(get_outgoingref) != SQLITE_ROW) {
            res        = 255;
            use_insert = 1;
        } else {
            res = sqlite3_column_int(get_outgoingref, 0);
        }
    }

    if (res >= 0) {
        ++res;
        if (res >= 256) {
            res = 0;
        }
        sqlite3_stmt* const outgoingref_stmt = use_insert ? ins_outgoingref_stmt : set_outgoingref_stmt;
        SCOPED_STMT(outgoingref);
        if (sqlite3_bind_int(outgoingref, 1, res) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind refid to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_bind_text(outgoingref, 2, ast_str_buffer(fullkey), ast_str_strlen(fullkey), SQLITE_STATIC) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(outgoingref) != SQLITE_DONE) {
            res = -1;
        }
    }

    return res;
}

int smsdb_outgoing_add(const char* id, const char* addr, int cnt, int ttl, int srr)
{
    int res = 0;

    SCOPED_TRANSACTION(dbtrans);
    SCOPED_STMT(put_outgoingmsg);

    if (sqlite3_bind_text(put_outgoingmsg, 1, id, strlen(id), SQLITE_STATIC) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind dev to stmt: %s\n", sqlite3_errmsg(smsdb));
        res = -1;
    } else if (sqlite3_bind_text(put_outgoingmsg, 2, addr, strlen(addr), SQLITE_STATIC) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind destination address to stmt: %s\n", sqlite3_errmsg(smsdb));
        res = -1;
    } else if (sqlite3_bind_int(put_outgoingmsg, 3, cnt) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind count to stmt: %s\n", sqlite3_errmsg(smsdb));
        res = -1;
    } else if (sqlite3_bind_int(put_outgoingmsg, 4, ttl) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind TTL to stmt: %s\n", sqlite3_errmsg(smsdb));
        res = -1;
    } else if (sqlite3_bind_int(put_outgoingmsg, 5, srr) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind SRR to stmt: %s\n", sqlite3_errmsg(smsdb));
        res = -1;
    } else if (sqlite3_step(put_outgoingmsg) != SQLITE_DONE) {
        res = -1;
    } else {
        res = sqlite3_last_insert_rowid(smsdb);
    }

    return res;
}

static int smsdb_outgoing_clear_nolock(int uid)
{
    int res = 0;

    {
        SCOPED_STMT(del_outgoingmsg);
        if (sqlite3_bind_int(del_outgoingmsg, 1, uid) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind UID to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(del_outgoingmsg) != SQLITE_DONE) {
            res = -1;
        }
    }

    {
        SCOPED_STMT(del_outgoingpart);
        if (sqlite3_bind_int(del_outgoingpart, 1, uid) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind UID to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(del_outgoingpart) != SQLITE_DONE) {
            res = -1;
        }
    }

    return res;
}

ssize_t smsdb_outgoing_clear(int uid, struct ast_str* dst)
{
    int res = 0;

    SCOPED_TRANSACTION(dbtrans);

    {
        SCOPED_STMT(get_dst);
        if (sqlite3_bind_int(get_dst, 1, uid) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(get_dst) != SQLITE_ROW) {
            res = -1;
        } else {
            ast_str_set(&dst, SMSDB_DST_MAX_LEN, "%s", (const char*)sqlite3_column_text(get_dst, 0));
        }
    }

    if (res >= 0 && smsdb_outgoing_clear_nolock(uid) < 0) {
        res = -1;
    }

    return res;
}

ssize_t smsdb_outgoing_part_put(int uid, int refid, struct ast_str* dst)
{
    int res = 0;
    int srr = 0;

    SCOPED_TRANSACTION(dbtrans);

    RAII_VAR(struct ast_str*, fullkey, ast_str_create(DBKEY_DEF_LEN), ast_free);

    {
        SCOPED_STMT(get_outgoingmsg);
        if (sqlite3_bind_int(get_outgoingmsg, 1, uid) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind UID to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(get_outgoingmsg) != SQLITE_ROW) {
            res = -2;
        } else {
            const char* dev = (const char*)sqlite3_column_text(get_outgoingmsg, 0);
            const char* dst = (const char*)sqlite3_column_text(get_outgoingmsg, 1);
            srr             = sqlite3_column_int(get_outgoingmsg, 2);

            const int fullkey_len = ast_str_set(&fullkey, DBKEY_MAX_LEN, "%s/%s/%d", dev, dst, refid);
            if (fullkey_len < 0) {
                ast_log(LOG_ERROR, "Unable to create key\n");
                res = -3;
            }
        }
    }

    if (res >= 0) {
        SCOPED_STMT(put_outgoingpart);
        if (sqlite3_bind_text(put_outgoingpart, 1, ast_str_buffer(fullkey), ast_str_strlen(fullkey), SQLITE_STATIC) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_bind_int(put_outgoingpart, 2, uid) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind UID to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(put_outgoingpart) != SQLITE_DONE) {
            res = -1;
        }
    }

    if (!srr) {
        res = -2;
    }

    // if no status report is requested, just count successfully inserted parts
    // reached the number of parts
    if (res >= 0) {
        SCOPED_STMT(cnt_all_outgoingpart);
        if (sqlite3_bind_int(cnt_all_outgoingpart, 1, uid) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(cnt_all_outgoingpart) != SQLITE_ROW) {
            res = -1;
        } else {
            const int cur = sqlite3_column_int(cnt_all_outgoingpart, 0);
            const int cnt = sqlite3_column_int(cnt_all_outgoingpart, 1);
            if (cur != cnt) {
                res = -2;
            }
        }
    }

    // get dst
    if (res >= 0) {
        SCOPED_STMT(get_dst);
        if (sqlite3_bind_int(get_dst, 1, uid) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(get_dst) != SQLITE_ROW) {
            res = -1;
        } else {
            ast_str_set(&dst, SMSDB_DST_MAX_LEN, "%s", sqlite3_column_text(get_dst, 0));
        }
    }

    // clear if everything is finished
    if (res >= 0 && smsdb_outgoing_clear_nolock(uid) < 0) {
        res = -1;
    }

    return res;
}

ssize_t smsdb_outgoing_part_status(const char* id, const char* addr, int mr, int st, int* status_all)
{
    int res = 0, partid, uid;

    RAII_VAR(struct ast_str*, fullkey, ast_str_create(DBKEY_DEF_LEN), ast_free);
    const int fullkey_len = ast_str_set(&fullkey, DBKEY_MAX_LEN, "%s/%s/%d", id, addr, mr);
    if (fullkey_len < 0) {
        ast_log(LOG_ERROR, "Key length must be less than %zu bytes\n", sizeof(fullkey));
        return -1;
    }

    SCOPED_TRANSACTION(dbtrans);

    {
        SCOPED_STMT(get_outgoingpart);
        if (sqlite3_bind_text(get_outgoingpart, 1, ast_str_buffer(fullkey), ast_str_strlen(fullkey), SQLITE_STATIC) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(get_outgoingpart) != SQLITE_ROW) {
            res = -1;
        } else {
            partid = sqlite3_column_int(get_outgoingpart, 0);
            uid    = sqlite3_column_int(get_outgoingpart, 1);
        }
    }

    // set status
    if (res >= 0) {
        SCOPED_STMT(set_outgoingpart);
        if (sqlite3_bind_int(set_outgoingpart, 1, st) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind status to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_bind_int(set_outgoingpart, 2, partid) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind ID to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(set_outgoingpart) != SQLITE_DONE) {
            res = -1;
        }
    }

    // get count
    if (res >= 0) {
        SCOPED_STMT(cnt_outgoingpart);
        if (sqlite3_bind_int(cnt_outgoingpart, 1, uid) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else if (sqlite3_step(cnt_outgoingpart) != SQLITE_ROW) {
            res = -1;
        } else {
            const int cur = sqlite3_column_int(cnt_outgoingpart, 0);
            const int cnt = sqlite3_column_int(cnt_outgoingpart, 1);
            if (cur != cnt) {
                res = -2;
            }
        }
    }

    // get status array
    if (res >= 0) {
        int i = 0;
        SCOPED_STMT(get_all_status);
        if (sqlite3_bind_int(get_all_status, 1, uid) != SQLITE_OK) {
            ast_log(LOG_WARNING, "Couldn't bind key to stmt: %s\n", sqlite3_errmsg(smsdb));
            res = -1;
        } else {
            while (sqlite3_step(get_all_status) == SQLITE_ROW) {
                status_all[i++] = sqlite3_column_int(get_all_status, 0);
            }
        }
        status_all[i] = -1;
    }

    // clear if everything is finished
    if (res >= 0 && smsdb_outgoing_clear_nolock(uid) < 0) {
        res = -1;
    }

    return res;
}

ssize_t smsdb_outgoing_purge_one(int* uid, struct ast_str* dst)
{
    int res = -1;

    SCOPED_TRANSACTION(dbtrans);

    {
        SCOPED_STMT(get_expired);
        if (sqlite3_step(get_expired) != SQLITE_ROW) {
            res = -1;
        } else {
            *uid = sqlite3_column_int(get_expired, 0);
            ast_str_set(&dst, SMSDB_DST_MAX_LEN, "%s", (const char*)sqlite3_column_text(get_expired, 1));
        }
    }

    if (res >= 0 && smsdb_outgoing_clear_nolock(*uid) < 0) {
        res = -1;
    }

    return res;
}

/*!
 * \internal
 * \brief Clean up resources on Asterisk shutdown
 */
void smsdb_atexit()
{
    SCOPED_MUTEX(dblock_lock, &dblock);
    clean_statements();
    if (sqlite3_close(smsdb) == SQLITE_OK) {
        smsdb = NULL;
    }
}

int smsdb_init()
{
    if (db_init()) {
        return -1;
    }

    return 0;
}
