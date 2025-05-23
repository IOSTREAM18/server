/*
   Copyright (c) 2004, 2012, Oracle and/or its affiliates.
   Copyright (c) 2010, 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335  USA */


#define MYSQL_LEX 1
#include "mariadb.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_priv.h"
#include "unireg.h"
#include "sp_head.h"
#include "sql_trigger.h"
#include "sql_parse.h"                          // parse_sql
#include "parse_file.h"
#include "sp.h"
#include "sql_base.h"
#include "sql_show.h"                     // append_definer, append_identifier
#include "sql_table.h"                    // build_table_filename,
                                          // check_n_cut_mysql50_prefix
#include "sql_db.h"                       // get_default_db_collation
#include "sql_handler.h"                  // mysql_ha_rm_tables
#include "sp_cache.h"                     // sp_invalidate_cache
#include <mysys_err.h>
#include "ddl_log.h"                      // ddl_log_state
#include "debug_sync.h"                   // DEBUG_SYNC
#include "debug.h"                        // debug_crash_here
#include "mysql/psi/mysql_sp.h"
#include "wsrep_mysqld.h"
#include <my_time.h>
#include <mysql_time.h>

/*************************************************************************/

static bool add_table_for_trigger_internal(THD *thd,
                                           const sp_name *trg_name,
                                           bool if_exists,
                                           TABLE_LIST **table,
                                           char *trn_path_buff);

/*
  Functions for TRIGGER_RENAME_PARAM
*/

void TRIGGER_RENAME_PARAM::reset()
{
  delete table.triggers;
  table.triggers= 0;
  free_root(&table.mem_root, MYF(0));
}


/**
  Trigger_creation_ctx -- creation context of triggers.
*/

class Trigger_creation_ctx : public Stored_program_creation_ctx,
                             public Sql_alloc
{
public:
  static Trigger_creation_ctx *create(THD *thd,
                                      const char *db_name,
                                      const char *table_name,
                                      const LEX_CSTRING *client_cs_name,
                                      const LEX_CSTRING *connection_cl_name,
                                      const LEX_CSTRING *db_cl_name);

  Trigger_creation_ctx(CHARSET_INFO *client_cs,
                       CHARSET_INFO *connection_cl,
                       CHARSET_INFO *db_cl)
    :Stored_program_creation_ctx(client_cs, connection_cl, db_cl)
  { }

  Stored_program_creation_ctx *clone(MEM_ROOT *mem_root) override
  {
    return new (mem_root) Trigger_creation_ctx(m_client_cs,
                                               m_connection_cl,
                                               m_db_cl);
  }

protected:
  Object_creation_ctx *create_backup_ctx(THD *thd) const override
  {
    return new Trigger_creation_ctx(thd);
  }

  Trigger_creation_ctx(THD *thd)
    :Stored_program_creation_ctx(thd)
  { }
};

/**************************************************************************
  Trigger_creation_ctx implementation.
**************************************************************************/

Trigger_creation_ctx *
Trigger_creation_ctx::create(THD *thd,
                             const char *db_name,
                             const char *table_name,
                             const LEX_CSTRING *client_cs_name,
                             const LEX_CSTRING *connection_cl_name,
                             const LEX_CSTRING *db_cl_name)
{
  CHARSET_INFO *client_cs;
  CHARSET_INFO *connection_cl;
  CHARSET_INFO *db_cl;

  bool invalid_creation_ctx= FALSE;
  myf utf8_flag= thd->get_utf8_flag();

  if (resolve_charset(client_cs_name->str,
                      thd->variables.character_set_client,
                      &client_cs, MYF(utf8_flag)))
  {
    sql_print_warning("Trigger for table '%s'.'%s': "
                      "invalid character_set_client value (%s).",
                      (const char *) db_name,
                      (const char *) table_name,
                      (const char *) client_cs_name->str);

    invalid_creation_ctx= TRUE;
  }

  if (resolve_collation(connection_cl_name->str,
                        thd->variables.collation_connection,
                        &connection_cl,MYF(utf8_flag)))
  {
    sql_print_warning("Trigger for table '%s'.'%s': "
                      "invalid collation_connection value (%s).",
                      (const char *) db_name,
                      (const char *) table_name,
                      (const char *) connection_cl_name->str);

    invalid_creation_ctx= TRUE;
  }

  if (resolve_collation(db_cl_name->str, NULL, &db_cl, MYF(utf8_flag)))
  {
    sql_print_warning("Trigger for table '%s'.'%s': "
                      "invalid database_collation value (%s).",
                      (const char *) db_name,
                      (const char *) table_name,
                      (const char *) db_cl_name->str);

    invalid_creation_ctx= TRUE;
  }

  if (invalid_creation_ctx)
  {
    push_warning_printf(thd,
                        Sql_condition::WARN_LEVEL_WARN,
                        ER_TRG_INVALID_CREATION_CTX,
                        ER_THD(thd, ER_TRG_INVALID_CREATION_CTX),
                        (const char *) db_name,
                        (const char *) table_name);
  }

  /*
    If we failed to resolve the database collation, load the default one
    from the disk.
  */

  if (!db_cl)
    db_cl= get_default_db_collation(thd, db_name);

  return new Trigger_creation_ctx(client_cs, connection_cl, db_cl);
}

/*************************************************************************/

static const LEX_CSTRING triggers_file_type=
  { STRING_WITH_LEN("TRIGGERS") };

const char * const TRG_EXT= ".TRG";

/**
  Table of .TRG file field descriptors.
  We have here only one field now because in nearest future .TRG
  files will be merged into .FRM files (so we don't need something
  like md5 or created fields).
*/
static File_option triggers_file_parameters[]=
{
  {
    { STRING_WITH_LEN("triggers") },
    my_offsetof(class Table_triggers_list, definitions_list),
    FILE_OPTIONS_STRLIST
  },
  {
    { STRING_WITH_LEN("sql_modes") },
    my_offsetof(class Table_triggers_list, definition_modes_list),
    FILE_OPTIONS_ULLLIST
  },
  {
    { STRING_WITH_LEN("definers") },
    my_offsetof(class Table_triggers_list, definers_list),
    FILE_OPTIONS_STRLIST
  },
  {
    { STRING_WITH_LEN("client_cs_names") },
    my_offsetof(class Table_triggers_list, client_cs_names),
    FILE_OPTIONS_STRLIST
  },
  {
    { STRING_WITH_LEN("connection_cl_names") },
    my_offsetof(class Table_triggers_list, connection_cl_names),
    FILE_OPTIONS_STRLIST
  },
  {
    { STRING_WITH_LEN("db_cl_names") },
    my_offsetof(class Table_triggers_list, db_cl_names),
    FILE_OPTIONS_STRLIST
  },
  {
    { STRING_WITH_LEN("created") },
    my_offsetof(class Table_triggers_list, hr_create_times),
    FILE_OPTIONS_ULLLIST
  },
  { { 0, 0 }, 0, FILE_OPTIONS_STRING }
};

File_option sql_modes_parameters=
{
  { STRING_WITH_LEN("sql_modes") },
  my_offsetof(class Table_triggers_list, definition_modes_list),
  FILE_OPTIONS_ULLLIST
};

/**
  This must be kept up to date whenever a new option is added to the list
  above, as it specifies the number of required parameters of the trigger in
  .trg file.
  This defines the maximum number of parameters that is read.  If there are
  more paramaters in the file they are ignored.  Less number of parameters
  is regarded as ok.
*/

static const int TRG_NUM_REQUIRED_PARAMETERS= 7;

/*
  Structure representing contents of .TRN file which are used to support
  database wide trigger namespace.
*/

struct st_trigname
{
  LEX_CSTRING trigger_table;
};

static const LEX_CSTRING trigname_file_type=
  { STRING_WITH_LEN("TRIGGERNAME") };

const char * const TRN_EXT= ".TRN";

static File_option trigname_file_parameters[]=
{
  {
    { STRING_WITH_LEN("trigger_table")},
    offsetof(struct st_trigname, trigger_table),
    FILE_OPTIONS_ESTRING
  },
  { { 0, 0 }, 0, FILE_OPTIONS_STRING }
};


class Handle_old_incorrect_sql_modes_hook: public Unknown_key_hook
{
private:
  const char *path;
public:
  Handle_old_incorrect_sql_modes_hook(const char *file_path)
    :path(file_path)
  {};
  bool process_unknown_string(const char *&unknown_key, uchar* base,
                                      MEM_ROOT *mem_root, const char *end) override;
};


class Handle_old_incorrect_trigger_table_hook: public Unknown_key_hook
{
public:
  Handle_old_incorrect_trigger_table_hook(const char *file_path,
                                          LEX_CSTRING *trigger_table_arg)
    :path(file_path), trigger_table_value(trigger_table_arg)
  {};
  bool process_unknown_string(const char *&unknown_key, uchar* base,
                                      MEM_ROOT *mem_root, const char *end) override;
private:
  const char *path;
  LEX_CSTRING *trigger_table_value;
};


/**
  An error handler that catches all non-OOM errors which can occur during
  parsing of trigger body. Such errors are ignored and corresponding error
  message is used to construct a more verbose error message which contains
  name of problematic trigger. This error message is later emitted when
  one tries to perform DML or some of DDL on this table.
  Also, if possible, grabs name of the trigger being parsed so it can be
  used to correctly drop problematic trigger.
*/
class Deprecated_trigger_syntax_handler : public Internal_error_handler
{
private:

  char m_message[MYSQL_ERRMSG_SIZE];
  const LEX_CSTRING *m_trigger_name;

public:

  Deprecated_trigger_syntax_handler() : m_trigger_name(NULL) {}

  bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char* sqlstate,
                                Sql_condition::enum_warning_level *level,
                                const char* message,
                                Sql_condition ** cond_hdl) override
  {
    if (sql_errno != EE_OUTOFMEMORY &&
        sql_errno != ER_OUT_OF_RESOURCES)
    {
      // Check if the current LEX contains a non-empty spname
      if(thd->lex->spname)
        m_trigger_name= &thd->lex->spname->m_name;
      else if (thd->lex->sphead)
      {
        /*
          Some SP statements, for example IF, create their own local LEX.
          All LEX instances are available in the LEX stack in sphead::m_lex.
          Let's find the one that contains a non-zero spname.
          Note, although a parse error has happened, the LEX instances
          in sphead::m_lex are not freed yet at this point. The first
          found non-zero spname contains the valid trigger name.
        */
        const sp_name *spname= thd->lex->sphead->find_spname_recursive();
        if (spname)
          m_trigger_name= &spname->m_name;
      }
      if (m_trigger_name)
        my_snprintf(m_message, sizeof(m_message),
                    ER_THD(thd, ER_ERROR_IN_TRIGGER_BODY),
                    m_trigger_name->str, message);
      else
        my_snprintf(m_message, sizeof(m_message),
                    ER_THD(thd, ER_ERROR_IN_UNKNOWN_TRIGGER_BODY), message);
      return true;
    }
    return false;
  }

  const LEX_CSTRING *get_trigger_name() { return m_trigger_name; }
  char *get_error_message() { return m_message; }
};


Trigger::~Trigger()
{
  sp_head::destroy(body);
}


/**
  Call a Table_triggers_list function for all triggers

  @return 0 ok
  @return # Something went wrong. Pointer to the trigger that mailfuncted
            returned
*/

Trigger* Table_triggers_list::for_all_triggers(Triggers_processor func,
                                               void *arg)
{
  for (uint i= 0; i < (uint)TRG_EVENT_MAX; i++)
  {
    for (uint j= 0; j < (uint)TRG_ACTION_MAX; j++)
    {
      for (Trigger *trigger= get_trigger(i,j) ;
           trigger ;
           trigger= trigger->next[i])
        if (is_the_right_most_event_bit(trigger->events, (trg_event_type)i) &&
            (trigger->*func)(arg))
        {
          (trigger->*func)(arg);
          return trigger;
        }
    }
  }
  return 0;
}


/**
  Create or drop trigger for table.

  @param thd     current thread context (including trigger definition in LEX)
  @param tables  table list containing one table for which trigger is created.
  @param create  whenever we create (TRUE) or drop (FALSE) trigger

  @note
    This function is mainly responsible for opening and locking of table and
    invalidation of all its instances in table cache after trigger creation.
    Real work on trigger creation/dropping is done inside Table_triggers_list
    methods.

  @todo
    TODO: We should check if user has TRIGGER privilege for table here.
    Now we just require SUPER privilege for creating/dropping because
    we don't have proper privilege checking for triggers in place yet.

  @retval
    FALSE Success
  @retval
    TRUE  error
*/

bool mysql_create_or_drop_trigger(THD *thd, TABLE_LIST *tables, bool create)
{
  /*
    FIXME: The code below takes too many different paths depending on the
    'create' flag, so that the justification for a single function
    'mysql_create_or_drop_trigger', compared to two separate functions
    'mysql_create_trigger' and 'mysql_drop_trigger' is not apparent.
    This is a good candidate for a minor refactoring.
  */
  TABLE *table;
  bool result= true, refresh_metadata= false;
  bool add_if_exists_to_binlog= false, action_executed= false;
  String stmt_query;
  bool lock_upgrade_done= FALSE;
  bool backup_of_table_list_done= 0;;
  MDL_ticket *mdl_ticket= NULL;
  MDL_request mdl_request_for_trn;
  Query_tables_list backup;
  DDL_LOG_STATE ddl_log_state, ddl_log_state_tmp_file;
  char trn_path_buff[FN_REFLEN];
  char path[FN_REFLEN + 1];

  DBUG_ENTER("mysql_create_or_drop_trigger");

  /* Charset of the buffer for statement must be system one. */
  stmt_query.set_charset(system_charset_info);
  bzero(&ddl_log_state, sizeof(ddl_log_state));
  bzero(&ddl_log_state_tmp_file, sizeof(ddl_log_state_tmp_file));

  /*
    QQ: This function could be merged in mysql_alter_table() function
    But do we want this ?
  */

  /*
    Note that once we will have check for TRIGGER privilege in place we won't
    need second part of condition below, since check_access() function also
    checks that db is specified.
  */
  if (!thd->lex->spname->m_db.length || (create && !tables->db.length))
  {
    my_error(ER_NO_DB_ERROR, MYF(0));
    DBUG_RETURN(TRUE);
  }

  /*
    We don't allow creating triggers on tables in the 'mysql' schema
  */
  if (create && tables->db.streq(MYSQL_SCHEMA_NAME))
  {
    my_error(ER_NO_TRIGGERS_ON_SYSTEM_SCHEMA, MYF(0));
    DBUG_RETURN(TRUE);
  }

  /*
    There is no DETERMINISTIC clause for triggers, so can't check it.
    But a trigger can in theory be used to do nasty things (if it supported
    DROP for example) so we do the check for privileges. For now there is
    already a stronger test right above; but when this stronger test will
    be removed, the test below will hold. Because triggers have the same
    nature as functions regarding binlogging: their body is implicitly
    binlogged, so they share the same danger, so trust_function_creators
    applies to them too.
  */
  if (!trust_function_creators                               &&
      (WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open()) &&
      !(thd->security_ctx->master_access & PRIV_LOG_BIN_TRUSTED_SP_CREATOR))
  {
    my_error(ER_BINLOG_CREATE_ROUTINE_NEED_SUPER, MYF(0));
    DBUG_RETURN(TRUE);
  }

  /* Protect against concurrent create/drop */
  MDL_REQUEST_INIT(&mdl_request_for_trn, MDL_key::TRIGGER,
                   create ? tables->db.str : thd->lex->spname->m_db.str,
                   thd->lex->spname->m_name.str,
                   MDL_EXCLUSIVE, MDL_EXPLICIT);
  if (thd->mdl_context.acquire_lock(&mdl_request_for_trn,
                                    thd->variables.lock_wait_timeout))
    goto end;

  if (!create)
  {
    bool if_exists= thd->lex->if_exists();

    /*
      Protect the query table list from the temporary and potentially
      destructive changes necessary to open the trigger's table.
    */
    backup_of_table_list_done= 1;
    thd->lex->reset_n_backup_query_tables_list(&backup);
    /*
      Restore Query_tables_list::sql_command, which was
      reset above, as the code that writes the query to the
      binary log assumes that this value corresponds to the
      statement that is being executed.
    */
    thd->lex->sql_command= backup.sql_command;

    if (opt_readonly &&
        !(thd->security_ctx->master_access & PRIV_IGNORE_READ_ONLY) &&
        !thd->slave_thread)
    {
      my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--read-only");
      goto end;
    }

    if (add_table_for_trigger_internal(thd, thd->lex->spname, if_exists, &tables,
                                       trn_path_buff))
      goto end;

    if (!tables)
    {
      DBUG_ASSERT(if_exists);
      /*
        Since the trigger does not exist, there is no associated table,
        and therefore :
        - no TRIGGER privileges to check,
        - no trigger to drop,
        - no table to lock/modify,
        so the drop statement is successful.
      */
      result= FALSE;
      /* Still, we need to log the query ... */
      stmt_query.set(thd->query(), thd->query_length(), system_charset_info);
      action_executed= 1;
      goto end;
    }
  }

  /*
    Check that the user has TRIGGER privilege on the subject table.
  */
  {
    bool err_status;
    TABLE_LIST **save_query_tables_own_last= thd->lex->query_tables_own_last;
    thd->lex->query_tables_own_last= 0;

    err_status= check_table_access(thd, TRIGGER_ACL, tables, FALSE, 1, FALSE);

    thd->lex->query_tables_own_last= save_query_tables_own_last;

    if (err_status)
      goto end;
  }

  WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, tables);

  /* We should have only one table in table list. */
  DBUG_ASSERT(tables->next_global == 0);

  build_table_filename(path, sizeof(path) - 1, tables->db.str, tables->alias.str, ".frm", 0);
  tables->required_type= dd_frm_type(NULL, path, NULL, NULL);

  /* We do not allow creation of triggers on temporary tables or sequence. */
  if (tables->required_type == TABLE_TYPE_SEQUENCE ||
      (create && thd->find_tmp_table_share(tables)))
  {
    my_error(ER_TRG_ON_VIEW_OR_TEMP_TABLE, MYF(0), tables->alias.str);
    goto end;
  }

  /* We also don't allow creation of triggers on views. */
  tables->required_type= TABLE_TYPE_NORMAL;
  /*
    Also prevent DROP TRIGGER from opening temporary table which might
    shadow the subject table on which trigger to be dropped is defined.
  */
  tables->open_type= OT_BASE_ONLY;

  /* Keep consistent with respect to other DDL statements */
  mysql_ha_rm_tables(thd, tables);

  if (thd->locked_tables_mode)
  {
    /* Under LOCK TABLES we must only accept write locked tables. */
    if (!(tables->table= find_table_for_mdl_upgrade(thd, tables->db.str,
                                                    tables->table_name.str,
                                                    NULL)))
      goto end;
  }
  else
  {
    tables->table= open_n_lock_single_table(thd, tables,
                                            TL_READ_NO_INSERT, 0);
    if (! tables->table)
    {
      if (!create && thd->get_stmt_da()->sql_errno() == ER_NO_SUCH_TABLE)
      {
        /* TRN file exists but table does not. Drop the orphan trigger */
        thd->clear_error();                     // Remove error from open
        goto drop_orphan_trn;
      }
      goto end;
    }
    tables->table->use_all_columns();
  }
  table= tables->table;

#ifdef WITH_WSREP
  /* Resolve should we replicate creation of the trigger.
     It should be replicated if storage engine(s) associated
     to trigger are replicated by Galera.
  */
  if (WSREP(thd) &&
      !wsrep_should_replicate_ddl_iterate(thd, tables))
    goto end;
#endif

  /* Later on we will need it to downgrade the lock */
  mdl_ticket= table->mdl_ticket;

  /*
    RENAME ensures that table is flushed properly and locked tables will
    be removed from the active transaction
  */
  if (wait_while_table_is_used(thd, table, HA_EXTRA_PREPARE_FOR_RENAME))
    goto end;

  lock_upgrade_done= TRUE;

  if (!table->triggers)
  {
    if (!create)
      goto drop_orphan_trn;
    if (!(table->triggers= new (&table->mem_root) Table_triggers_list(table)))
      goto end;
  }

#if defined WITH_WSREP && defined ENABLED_DEBUG_SYNC
  DBUG_EXECUTE_IF("sync.mdev_20225",
                  {
                    const char act[]=
                      "now "
                      "wait_for signal.mdev_20225_continue";
                    DBUG_ASSERT(!debug_sync_set_action(thd,
                                                       STRING_WITH_LEN(act)));
                  };);
#endif /* WITH_WSREP && ENABLED_DEBUG_SYNC */

  if (create)
    result= table->triggers->create_trigger(thd, tables, &stmt_query,
                                            &ddl_log_state,
                                            &ddl_log_state_tmp_file);
  else
  {
    result= table->triggers->drop_trigger(thd, tables,
                                          &thd->lex->spname->m_name,
                                          &stmt_query,
                                          &ddl_log_state);
    if (result)
    {
      thd->clear_error();                     // Remove error from drop trigger
      goto drop_orphan_trn;
    }
  }
  action_executed= 1;

  refresh_metadata= TRUE;

end:
  if (!result && action_executed)
  {
    ulonglong save_option_bits= thd->variables.option_bits;
    backup_log_info ddl_log;

    debug_crash_here("ddl_log_drop_before_binlog");
    if (add_if_exists_to_binlog)
      thd->variables.option_bits|= OPTION_IF_EXISTS;
    thd->binlog_xid= thd->query_id;
    ddl_log_update_xid(&ddl_log_state, thd->binlog_xid);
    result= write_bin_log(thd, TRUE, stmt_query.ptr(),
                          stmt_query.length());
    thd->binlog_xid= 0;
    thd->variables.option_bits= save_option_bits;
    debug_crash_here("ddl_log_drop_after_binlog");

    bzero(&ddl_log, sizeof(ddl_log));
    if (create)
      ddl_log.query= { C_STRING_WITH_LEN("CREATE") };
    else
      ddl_log.query= { C_STRING_WITH_LEN("DROP") };
    ddl_log.org_storage_engine_name= { C_STRING_WITH_LEN("TRIGGER") };
    ddl_log.org_database=     thd->lex->spname->m_db;
    ddl_log.org_table=        thd->lex->spname->m_name;
    backup_log_ddl(&ddl_log);
  }
  ddl_log_complete(&ddl_log_state);
  debug_crash_here("ddl_log_drop_before_delete_tmp");
  /* delete any created log files */
  result|= ddl_log_revert(thd, &ddl_log_state_tmp_file);

  if (mdl_request_for_trn.ticket)
    thd->mdl_context.release_lock(mdl_request_for_trn.ticket);

  if (refresh_metadata)
  {
    close_all_tables_for_name(thd, table->s, HA_EXTRA_NOT_USED, NULL);

    /*
      Reopen the table if we were under LOCK TABLES.
      Ignore the return value for now. It's better to
      keep master/slave in consistent state.
    */
    if (thd->locked_tables_list.reopen_tables(thd, false))
      thd->clear_error();

    /*
      Invalidate SP-cache. That's needed because triggers may change list of
      pre-locking tables.
    */
    sp_cache_invalidate();
  }
  /*
    If we are under LOCK TABLES we should restore original state of
    meta-data locks. Otherwise all locks will be released along
    with the implicit commit.
  */
  if (thd->locked_tables_mode && tables && lock_upgrade_done)
    mdl_ticket->downgrade_lock(MDL_SHARED_NO_READ_WRITE);

  /* Restore the query table list. Used only for drop trigger. */
  if (backup_of_table_list_done)
    thd->lex->restore_backup_query_tables_list(&backup);

  if (!result)
  {
    my_ok(thd);
    /* Drop statistics for this stored program from performance schema. */
    MYSQL_DROP_SP(SP_TYPE_TRIGGER,
                  thd->lex->spname->m_db.str, static_cast<uint>(thd->lex->spname->m_db.length),
                  thd->lex->spname->m_name.str, static_cast<uint>(thd->lex->spname->m_name.length));
  }

  DBUG_RETURN(result);

#ifdef WITH_WSREP
wsrep_error_label:
  DBUG_ASSERT(result == 1);
  goto end;
#endif

drop_orphan_trn:
  my_error(ER_REMOVED_ORPHAN_TRIGGER, MYF(ME_WARNING),
           thd->lex->spname->m_name.str, tables->table_name.str);
  mysql_file_delete(key_file_trg, trn_path_buff, MYF(0));
  result= thd->is_error();
  add_if_exists_to_binlog= 1;
  action_executed= 1;                           // Ensure query is binlogged
  stmt_query.set(thd->query(), thd->query_length(), system_charset_info);
  goto end;
}


/**
  Build stmt_query to write it in the bin-log, the statement to write in
  the trigger file and the trigger definer.

  @param thd           current thread context (including trigger definition in
                       LEX)
  @param tables        table list containing one open table for which the
                       trigger is created.
  @param[out] stmt_query    after successful return, this string contains
                            well-formed statement for creation this trigger.
  @param[out] trigger_def  query to be stored in trigger file. As stmt_query,
		           but without "OR REPLACE" and no FOLLOWS/PRECEDES.
  @param[out] trg_definer         The trigger definer.
  @param[out] trg_definer_holder  Used as a buffer for definer.

  @note
    - Assumes that trigger name is fully qualified.
    - NULL-string means the following LEX_STRING instance:
    { str = 0; length = 0 }.
    - In other words, definer_user and definer_host should contain
    simultaneously NULL-strings (non-SUID/old trigger) or valid strings
    (SUID/new trigger).
*/

static void build_trig_stmt_query(THD *thd, TABLE_LIST *tables,
                                  String *stmt_query, String *trigger_def,
                                  LEX_CSTRING *trg_definer,
                                  char trg_definer_holder[])
{
  LEX_CSTRING stmt_definition;
  LEX *lex= thd->lex;
  size_t prefix_trimmed, suffix_trimmed;
  size_t original_length;

  /*
    Create a query with the full trigger definition.
    The original query is not appropriate, as it can miss the DEFINER=XXX part.
  */
  stmt_query->append(STRING_WITH_LEN("CREATE "));

  trigger_def->copy(*stmt_query);

  if (lex->create_info.or_replace())
    stmt_query->append(STRING_WITH_LEN("OR REPLACE "));

  if (lex->sphead->suid() != SP_IS_NOT_SUID)
  {
    /* SUID trigger */
    lex->definer->set_lex_string(trg_definer, trg_definer_holder);
    append_definer(thd, stmt_query, &lex->definer->user, &lex->definer->host);
    append_definer(thd, trigger_def, &lex->definer->user, &lex->definer->host);
  }
  else
  {
    *trg_definer= empty_clex_str;
  }


  /* Create statement for binary logging */
  stmt_definition.str=    lex->stmt_definition_begin;
  stmt_definition.length= (lex->stmt_definition_end -
                           lex->stmt_definition_begin);
  original_length= stmt_definition.length;
  trim_whitespace(thd->charset(), &stmt_definition, &prefix_trimmed);
  suffix_trimmed= original_length - stmt_definition.length - prefix_trimmed;

  stmt_query->append(stmt_definition.str, stmt_definition.length);

  /* Create statement for storing trigger (without trigger order) */
  if (lex->trg_chistics.ordering_clause == TRG_ORDER_NONE)
  {
    /*
      Not that here stmt_definition doesn't end with a \0, which is
      normally expected from a LEX_CSTRING
    */
    trigger_def->append(stmt_definition.str, stmt_definition.length);
  }
  else
  {
    /* Copy data before FOLLOWS/PRECEDES trigger_name */
    trigger_def->append(stmt_definition.str,
                        (lex->trg_chistics.ordering_clause_begin -
                         lex->stmt_definition_begin) - prefix_trimmed);
    /* Copy data after FOLLOWS/PRECEDES trigger_name */
    trigger_def->append(stmt_definition.str +
                        (lex->trg_chistics.ordering_clause_end -
                         lex->stmt_definition_begin)
                        - prefix_trimmed,
                        (lex->stmt_definition_end -
                         lex->trg_chistics.ordering_clause_end) -
                        suffix_trimmed);
  }
}


/**
  Visit every Item_trigger_field object associated with a trigger
  and run the code supplied in the last argument, passing
  the Item_trigger_fgield object being visited.

  @param trg_table_fields  Item_trigger_field objects owned by a trigger
  @param fn                a function to invoke for every Item_trigger_field
                           object

  @return false on success, true on failure.
*/

template <typename FN>
static
bool iterate_trigger_fields_and_run_func(
  SQL_I_List<SQL_I_List<Item_trigger_field> > &trg_table_fields,
  FN fn
  )
{
  for (SQL_I_List<Item_trigger_field>
         *trg_fld_lst= trg_table_fields.first;
       trg_fld_lst;
       trg_fld_lst= trg_fld_lst->first->next_trig_field_list)
  {
    for (Item_trigger_field *trg_field= trg_fld_lst->first;
         trg_field;
         trg_field= trg_field->next_trg_field)
    {
      if (fn(trg_field))
        return true;
    }
  }
  return false;
}

/**
  Create trigger for table.

  @param thd           current thread context (including trigger definition in
                       LEX)
  @param tables        table list containing one open table for which the
                       trigger is created.
  @param[out] stmt_query    after successful return, this string contains
                            well-formed statement for creation this trigger.

  @note
    - Assumes that trigger name is fully qualified.
    - NULL-string means the following LEX_STRING instance:
    { str = 0; length = 0 }.
    - In other words, definer_user and definer_host should contain
    simultaneously NULL-strings (non-SUID/old trigger) or valid strings
    (SUID/new trigger).

  @retval
    False   success
  @retval
    True    error
*/

bool Table_triggers_list::create_trigger(THD *thd, TABLE_LIST *tables,
                                         String *stmt_query,
                                         DDL_LOG_STATE *ddl_log_state,
                                         DDL_LOG_STATE *ddl_log_state_tmp_file)
{
  LEX *lex= thd->lex;
  TABLE *table= tables->table;
  char file_buff[FN_REFLEN], trigname_buff[FN_REFLEN];
  char backup_file_buff[FN_REFLEN];
  char trg_definer_holder[USER_HOST_BUFF_SIZE];
  LEX_CSTRING backup_name= { backup_file_buff, 0 };
  LEX_CSTRING file, trigname_file;
  struct st_trigname trigname;
  String trigger_definition;
  Trigger *trigger= 0;
  int error;
  bool trigger_exists;
  DBUG_ENTER("create_trigger");

  if (check_for_broken_triggers())
    DBUG_RETURN(true);

  /* Trigger must be in the same schema as target table. */
  if (!table->s->db.streq(lex->spname->m_db))
  {
    my_error(ER_TRG_IN_WRONG_SCHEMA, MYF(0));
    DBUG_RETURN(true);
  }

  if (sp_process_definer(thd))
    DBUG_RETURN(true);

  /*
    Let us check if all references to fields in old/new versions of row in
    this trigger are ok.

    NOTE: We do it here more from ease of use standpoint. We still have to
    do some checks on each execution. E.g. we can catch privilege changes
    only during execution. Also in near future, when we will allow access
    to other tables from trigger we won't be able to catch changes in other
    tables...

    Since we don't plan to access to contents of the fields it does not
    matter that we choose for both OLD and NEW values the same versions
    of Field objects here.
  */
  old_field= new_field= table->field;

  if (iterate_trigger_fields_and_run_func(
        lex->sphead->m_trg_table_fields,
        [thd, table] (Item_trigger_field* trg_field)
        {
          /*
            NOTE: now we do not check privileges at CREATE TRIGGER time.
            This will be changed in the future.
          */
          trg_field->setup_field(thd, table, nullptr);

          return trg_field->fix_fields_if_needed(thd, (Item **)0);
        }
     ))
    DBUG_RETURN(true);

  /* Ensure anchor trigger exists */
  if (lex->trg_chistics.ordering_clause != TRG_ORDER_NONE)
  {
    if (!(trigger= find_trigger(&lex->trg_chistics.anchor_trigger_name, 0)) ||
        /*
          check that every event listed for the trigger being created is also
          specified for anchored trigger
        */
        !is_subset_of_trg_events(trigger->events, lex->trg_chistics.events) ||
        trigger->action_time != lex->trg_chistics.action_time)
    {
      my_error(ER_REFERENCED_TRG_DOES_NOT_EXIST, MYF(0),
               lex->trg_chistics.anchor_trigger_name.str);
      DBUG_RETURN(true);
    }
  }

  /*
    Here we are creating file with triggers and save all triggers in it.
    sql_create_definition_file() files handles renaming and backup of older
    versions
  */
  file.length= build_table_filename(file_buff, FN_REFLEN - 1,
                                    tables->db.str, tables->table_name.str,
                                    TRG_EXT, 0);
  file.str= file_buff;
  trigname_file.length= build_table_filename(trigname_buff, FN_REFLEN-1,
                                             tables->db.str,
                                             lex->spname->m_name.str,
                                             TRN_EXT, 0);
  trigname_file.str= trigname_buff;

  /* Use the filesystem to enforce trigger namespace constraints. */
  trigger_exists= !access(trigname_file.str, F_OK);

  ddl_log_create_trigger(ddl_log_state, &tables->db, &tables->table_name,
                         &lex->spname->m_name,
                         trigger_exists || table->triggers->count ?
                         DDL_CREATE_TRIGGER_PHASE_DELETE_COPY :
                         DDL_CREATE_TRIGGER_PHASE_NO_OLD_TRIGGER);

  /* Make a backup of the .TRG file that we can restore in case of crash */
  if (table->triggers->count &&
      (sql_backup_definition_file(&file, &backup_name) ||
       ddl_log_delete_tmp_file(ddl_log_state_tmp_file, &backup_name,
                               ddl_log_state)))
    DBUG_RETURN(true);

  if (trigger_exists)
  {
    if (lex->create_info.or_replace())
    {
      LEX_CSTRING *sp_name= &thd->lex->spname->m_name; // alias

      /* Make a backup of the .TRN file that we can restore in case of crash */
      if (sql_backup_definition_file(&trigname_file, &backup_name) ||
          ddl_log_delete_tmp_file(ddl_log_state_tmp_file, &backup_name,
                                  ddl_log_state))
        DBUG_RETURN(true);
      ddl_log_update_phase(ddl_log_state, DDL_CREATE_TRIGGER_PHASE_OLD_COPIED);

      /*
        The following can fail if the trigger is for another table or
        there exists a .TRN file but there was no trigger for it in
        the .TRG file
      */
      if (unlikely(drop_trigger(thd, tables, sp_name, 0, 0)))
        DBUG_RETURN(true);
    }
    else if (lex->create_info.if_not_exists())
    {
      strxnmov(trigname_buff, sizeof(trigname_buff) - 1, tables->db.str, ".",
               lex->spname->m_name.str, NullS);
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_TRG_ALREADY_EXISTS,
                          ER_THD(thd, ER_TRG_ALREADY_EXISTS),
                          trigname_buff);
      LEX_CSTRING trg_definer_tmp;
      String trigger_def;

      /*
        Log query with IF NOT EXISTS to binary log. This is in line with
        CREATE TABLE IF NOT EXISTS.
      */
      build_trig_stmt_query(thd, tables, stmt_query, &trigger_def,
                            &trg_definer_tmp, trg_definer_holder);
      DBUG_RETURN(false);
    }
    else
    {
      strxnmov(trigname_buff, sizeof(trigname_buff) - 1, tables->db.str, ".",
               lex->spname->m_name.str, NullS);
      my_error(ER_TRG_ALREADY_EXISTS, MYF(0), trigname_buff);
      DBUG_RETURN(true);
    }
  }
  else
  {
    if (table->triggers->count)
      ddl_log_update_phase(ddl_log_state, DDL_CREATE_TRIGGER_PHASE_OLD_COPIED);
  }

  trigname.trigger_table= tables->table_name;

  /*
    We are not using lex->sphead here as an argument to Trigger() as we are
    going to access lex->sphead later in build_trig_stmt_query()
  */
  if (!(trigger= new (&table->mem_root) Trigger(this, 0)))
    goto err;

  /* Time with in microseconds */
  trigger->hr_create_time= make_hr_time(thd->query_start(),
                                        thd->query_start_sec_part());

  /* Create trigger_name.TRN file to ensure trigger name is unique */
  if (sql_create_definition_file(NULL, &trigname_file, &trigname_file_type,
                                 (uchar*)&trigname, trigname_file_parameters))
  {
    delete trigger;
    trigger= 0;
    goto err;
  }

  /* Populate the trigger object */

  trigger->sql_mode= thd->variables.sql_mode;
  build_trig_stmt_query(thd, tables, stmt_query, &trigger_definition,
                        &trigger->definer, trg_definer_holder);

  trigger->definition.str=    trigger_definition.c_ptr();
  trigger->definition.length= trigger_definition.length();

  /*
    Fill character set information:
      - client character set contains charset info only;
      - connection collation contains pair {character set, collation};
      - database collation contains pair {character set, collation};
  */
  trigger->client_cs_name= thd->charset()->cs_name;
  trigger->connection_cl_name= thd->variables.collation_connection->coll_name;
  trigger->db_cl_name= get_default_db_collation(thd, tables->db.str)->coll_name;
  trigger->name= Lex_ident_trigger(lex->spname->m_name);


  /* Add trigger in it's correct place */
  add_trigger(lex->trg_chistics.events,
              lex->trg_chistics.action_time,
              lex->trg_chistics.ordering_clause,
              Lex_ident_trigger(lex->trg_chistics.anchor_trigger_name),
              trigger);

  /* Create trigger definition file .TRG */
  if (unlikely(create_lists_needed_for_files(thd->mem_root)))
    goto err;

  debug_crash_here("ddl_log_create_before_create_trigger");
  error= sql_create_definition_file(NULL, &file, &triggers_file_type,
                                    (uchar*)this, triggers_file_parameters);
  debug_crash_here("ddl_log_create_after_create_trigger");

  if (!error)
    DBUG_RETURN(false);

err:
  DBUG_PRINT("error",("create trigger failed"));
  if (trigger)
  {
    my_debug_put_break_here();
    /* Delete trigger from trigger list if it exists */
    find_trigger(&trigger->name, 1);
    /* Free trigger memory */
    delete trigger;
  }

  /* Recover the old .TRN and .TRG files & delete backup files */
  ddl_log_revert(thd, ddl_log_state);
  /* All backup files are now deleted */
  ddl_log_complete(ddl_log_state_tmp_file);
  DBUG_RETURN(true);
}


/**
   Empty all list used to load and create .TRG file
*/

void Table_triggers_list::empty_lists()
{
  definitions_list.empty();
  definition_modes_list.empty();
  definers_list.empty();
  client_cs_names.empty();
  connection_cl_names.empty();
  db_cl_names.empty();
  hr_create_times.empty();
}


/**
   Create list of all trigger parameters for sql_create_definition_file()
*/

struct create_lists_param
{
  MEM_ROOT *root;
};


bool Table_triggers_list::create_lists_needed_for_files(MEM_ROOT *root)
{
  create_lists_param param;

  empty_lists();
  param.root= root;

  return for_all_triggers(&Trigger::add_to_file_list, &param);
}


bool Trigger::add_to_file_list(void* param_arg)
{
  create_lists_param *param= (create_lists_param*) param_arg;
  MEM_ROOT *mem_root= param->root;

  if (base->definitions_list.push_back(&definition, mem_root) ||
      base->definition_modes_list.push_back(&sql_mode, mem_root) ||
      base->definers_list.push_back(&definer, mem_root) ||
      base->client_cs_names.push_back(&client_cs_name, mem_root) ||
      base->connection_cl_names.push_back(&connection_cl_name, mem_root) ||
      base->db_cl_names.push_back(&db_cl_name, mem_root) ||
      base->hr_create_times.push_back(&hr_create_time.val, mem_root))
    return 1;
  return 0;
}


/**
  Check that there is a column in ON UPDATE trigger matching with some of
  the table's column from UPDATE statement.

  @param  fields to be updated by the UPDATE statement

  @return true in case there is a column in the target table that matches one
          of columns specified by a trigger definition or no columns were
          specified for the trigger at all, else return false.

*/

bool Trigger::match_updatable_columns(List<Item> &fields)
{
  DBUG_ASSERT(is_trg_event_on(events, TRG_EVENT_UPDATE));

  /*
    No table columns were specified in OF col1, col2 ... colN of
    the statement CREATE TRIGGER BEFORE/AFTER UPDATE. It means that this
    ON UPDATE trigger can't be fired on every UPDATE statement involving
    the target table.
  */
  if (!updatable_columns || updatable_columns->is_empty())
    return true;

  List_iterator_fast<Item> fields_it(fields);
  List_iterator_fast<LEX_CSTRING> columns_it(*updatable_columns);
  LEX_CSTRING *column_name;
  Item_field *field;

  /*
    Stop search on the first matching of a column taken from UPDATE statement
    with any column listed in trigger column list.
  */
  while ((field= (Item_field*)fields_it++))
  {
    while ((column_name= columns_it++))
    {
      if (field->field_name.streq(*column_name))
        return true;
    }
  }

  return false;
}


/**
  Deletes the .TRG file for a table.

  @param path         char buffer of size FN_REFLEN to be used
                      for constructing path to .TRG file.
  @param db           table's database name
  @param table_name   table's name

  @retval
    False   success
  @retval
    True    error
*/

static bool rm_trigger_file(char *path, const LEX_CSTRING *db,
                            const LEX_CSTRING *table_name, myf MyFlags)
{
  build_table_filename(path, FN_REFLEN-1, db->str, table_name->str, TRG_EXT, 0);
  return mysql_file_delete(key_file_trg, path, MyFlags);
}


/**
  Deletes the .TRN file for a trigger.

  @param path         char buffer of size FN_REFLEN to be used
                      for constructing path to .TRN file.
  @param db           trigger's database name
  @param trigger_name trigger's name

  @retval
    False   success
  @retval
    True    error
*/

bool rm_trigname_file(char *path, const LEX_CSTRING *db,
                      const LEX_CSTRING *trigger_name, myf MyFlags)
{
  build_table_filename(path, FN_REFLEN - 1, db->str, trigger_name->str,
                       TRN_EXT, 0);
  return mysql_file_delete(key_file_trn, path, MyFlags);
}


/**
  Helper function that saves .TRG file for Table_triggers_list object.

  @param triggers    Table_triggers_list object for which file should be saved
  @param db          Name of database for subject table
  @param table_name  Name of subject table

  @retval
    FALSE  Success
  @retval
    TRUE   Error
*/

bool Table_triggers_list::save_trigger_file(THD *thd, const LEX_CSTRING *db,
                                            const LEX_CSTRING *table_name)
{
  char file_buff[FN_REFLEN];
  LEX_CSTRING file;
  DBUG_ENTER("Table_triggers_list::save_trigger_file");

  if (create_lists_needed_for_files(thd->mem_root))
    DBUG_RETURN(true);

  file.length= build_table_filename(file_buff, FN_REFLEN - 1, db->str, table_name->str,
                                    TRG_EXT, 0);
  file.str= file_buff;
  DBUG_RETURN(sql_create_definition_file(NULL, &file, &triggers_file_type,
                                         (uchar*) this,
                                         triggers_file_parameters));
}


/**
  Find a trigger with a given name

  @param name	 		Name of trigger
  @param remove_from_list	If set, remove trigger if found
*/

Trigger *Table_triggers_list::find_trigger(const LEX_CSTRING *name,
                                           bool remove_from_list)
{
  Trigger *trigger = nullptr;

  for (uint i= 0; i < (uint)TRG_EVENT_MAX; i++)
  {
    for (uint j= 0; j < (uint)TRG_ACTION_MAX; j++)
    {
      Trigger **parent;

      for (parent= &triggers[i][j];
           (trigger= *parent);
           parent= &trigger->next[i])
      {
        if (trigger->name.streq(*name))
        {
          if (remove_from_list)
          {
            *parent= trigger->next[i];
            count--;
            /*
              in case only one event left or was assigned to this trigger
              return it, else continue iterations to remove the trigger
              from all events entries.
            */
            if (trigger->events != (1 << i))
            {
              /*
                Turn off event bits in the mask as the trigger is removed
                from the array for corresponding trigger event action.
                Eventually, we come to the last event this trigger is
                associated to. The associated trigger be returned from
                the method and finally deleted.
              */
              trigger->events &= ~(1 << i);
              continue;
            }
          }
          return trigger;
        }
      }
    }
  }
  /*
    We come to this point if either remove_from_list == true and
    the trigger is associated with multiple events, or there is no a trigger
    with requested name.
  */
  return trigger;
}


/**
  Drop trigger for table.

  @param thd           current thread context
                       (including trigger definition in LEX)
  @param tables        table list containing one open table for which trigger
                       is dropped.
  @param[out] stmt_query    after successful return, this string contains
                            well-formed statement for creation this trigger.

  @todo
    Probably instead of removing .TRG file we should move
    to archive directory but this should be done as part of
    parse_file.cc functionality (because we will need it
    elsewhere).

  @retval
    False   success
  @retval
    True    error
*/

bool Table_triggers_list::drop_trigger(THD *thd, TABLE_LIST *tables,
                                       LEX_CSTRING *sp_name,
                                       String *stmt_query,
                                       DDL_LOG_STATE *ddl_log_state)
{
  char path[FN_REFLEN];
  Trigger *trigger;
  DBUG_ENTER("Table_triggers_list::drop_trigger");

  if (stmt_query)
    stmt_query->set(thd->query(), thd->query_length(), stmt_query->charset());

  /* Find and delete trigger from list */
  if (!(trigger= find_trigger(sp_name, true)))
  {
    my_message(ER_TRG_DOES_NOT_EXIST, ER_THD(thd, ER_TRG_DOES_NOT_EXIST),
               MYF(0));
    DBUG_RETURN(1);
  }
  delete trigger;

  if (ddl_log_state)
  {
    LEX_CSTRING query= {0,0};
    if (stmt_query)
    {
      /* This code is executed in case of DROP TRIGGER */
      query = { thd->query(), thd->query_length() };
    }
    if (ddl_log_drop_trigger(ddl_log_state, &tables->db, &tables->table_name,
                             sp_name, &query))
      goto err;
  }
  debug_crash_here("ddl_log_drop_before_drop_trigger");

  if (!count)                                   // If no more triggers
  {
    /*
      It is safe to remove the trigger file.  If something goes wrong during
      drop or create ddl_log recovery will ensure that all related
      trigger files are deleted or the original ones are restored.
    */
    if (rm_trigger_file(path, &tables->db, &tables->table_name, MYF(MY_WME)))
      goto err;
  }
  else
  {
    if (save_trigger_file(thd, &tables->db, &tables->table_name))
      goto err;
  }

  debug_crash_here("ddl_log_drop_before_drop_trn");

  if (rm_trigname_file(path, &tables->db, sp_name, MYF(MY_WME)))
    goto err;

  debug_crash_here("ddl_log_drop_after_drop_trigger");

  DBUG_RETURN(0);

err:
  DBUG_RETURN(1);
}


Table_triggers_list::~Table_triggers_list()
{
  DBUG_ENTER("Table_triggers_list::~Table_triggers_list");

  /*
    Iterate over trigger events in descending order to delete only the last
    instance of the Trigger class in case there are several events associated
    with the trigger.
  */
  for (int i= (int)TRG_EVENT_MAX - 1; i >= 0; i--)
  {
    for (uint j= 0; j < (uint)TRG_ACTION_MAX; j++)
    {
      Trigger *next, *trigger;
      for (trigger= get_trigger(i,j) ; trigger ; trigger= next)
      {
        next= trigger->next[i];
        /*
          Since iteration along triggers is performed in descending order
          deleting an instance of the Trigger class for the right most event
          bit guarantees that the instance is deleted only once.
        */
        if (is_the_right_most_event_bit(trigger->events, (trg_event_type)i))
          delete trigger;
      }
    }
  }

  /* Free blobs used in insert */
  if (record0_field)
    for (Field **fld_ptr= record0_field; *fld_ptr; fld_ptr++)
      (*fld_ptr)->free();

  if (record1_field)
    for (Field **fld_ptr= record1_field; *fld_ptr; fld_ptr++)
      delete *fld_ptr;

  DBUG_VOID_RETURN;
}


/**
  Prepare array of Field objects referencing to TABLE::record[1] instead
  of record[0] (they will represent OLD.* row values in ON UPDATE trigger
  and in ON DELETE trigger which will be called during REPLACE execution).

  @param table   pointer to TABLE object for which we are creating fields.

  @retval
    False   success
  @retval
    True    error
*/

bool Table_triggers_list::prepare_record_accessors(TABLE *table)
{
  Field **fld, **trg_fld;

  if ((has_triggers(TRG_EVENT_INSERT,TRG_ACTION_BEFORE) ||
       has_triggers(TRG_EVENT_UPDATE,TRG_ACTION_BEFORE)) &&
      (table->s->stored_fields != table->s->null_fields))

  {
    int null_bytes= (table->s->fields - table->s->null_fields + 7)/8;
    if (!(extra_null_bitmap= (uchar*)alloc_root(&table->mem_root, 2*null_bytes)))
      return 1;
    extra_null_bitmap_init= extra_null_bitmap + null_bytes;
    if (!(record0_field= (Field **)alloc_root(&table->mem_root,
                                              (table->s->fields + 1) *
                                              sizeof(Field*))))
      return 1;

    uchar *null_ptr= extra_null_bitmap;
    uchar null_bit= 1;
    for (fld= table->field, trg_fld= record0_field; *fld; fld++, trg_fld++)
    {
      if (!(*fld)->null_ptr && !(*fld)->vcol_info && !(*fld)->vers_sys_field())
      {
        Field *f;
        if (!(f= *trg_fld= (*fld)->make_new_field(&table->mem_root, table,
                                                  table == (*fld)->table)))
          return 1;

        f->flags= (*fld)->flags;
        f->invisible= (*fld)->invisible;
        f->null_ptr= null_ptr;
        f->null_bit= null_bit;
        if (null_bit == 128)
          null_ptr++, null_bit= 1;
        else
          null_bit*= 2;
        if (f->flags & NO_DEFAULT_VALUE_FLAG)
          f->set_null();
        else
          f->set_notnull();
      }
      else
        *trg_fld= *fld;
    }
    *trg_fld= 0;
    DBUG_ASSERT(null_ptr <= extra_null_bitmap + null_bytes);
    memcpy(extra_null_bitmap_init, extra_null_bitmap, null_bytes);
  }
  else
  {
    record0_field= table->field;
  }

  if (has_triggers(TRG_EVENT_UPDATE,TRG_ACTION_BEFORE) ||
      has_triggers(TRG_EVENT_UPDATE,TRG_ACTION_AFTER)  ||
      has_triggers(TRG_EVENT_DELETE,TRG_ACTION_BEFORE) ||
      has_triggers(TRG_EVENT_DELETE,TRG_ACTION_AFTER))
  {
    if (!(record1_field= (Field **)alloc_root(&table->mem_root,
                                              (table->s->fields + 1) *
                                              sizeof(Field*))))
      return 1;

    for (fld= table->field, trg_fld= record1_field; *fld; fld++, trg_fld++)
    {
      if (!(*trg_fld= (*fld)->make_new_field(&table->mem_root, table,
                                             table == (*fld)->table)))
        return 1;
      (*trg_fld)->move_field_offset((my_ptrdiff_t)(table->record[1] -
                                                   table->record[0]));
    }
    *trg_fld= 0;
  }
  return 0;
}


/**
  Deep copy of on update columns list created on parsing a trigger definition.
  The destination list and its elements are allocated on table's memory root.

  @param table_mem_root  table mem_root from where a memory is allocated.
  @param [out]  dst_col_names  destination list where to copy an original one
  @param  src_col_names  source list that has to be copied

  @return false on success, true on OOM error
*/
static bool
copy_on_update_columns_list(MEM_ROOT *table_mem_root,
                            List<LEX_CSTRING> **dst_col_names,
                            List<LEX_CSTRING> *src_col_names)
{
  if (!src_col_names)
  {
    *dst_col_names= nullptr;
    return false;
  }

  /*
    In case the clause <OF column_list> is present in definition of a trigger,
    the list lex.trg_chistics.on_update_col_names mustn't be empty.
    For the case where this clause is missed, the pointer
      lex.trg_chistics.on_update_col_names
    itself has nullptr value. So, do assert check here that the list is not
    empty.
  */
  DBUG_ASSERT(!src_col_names->is_empty());

  List<LEX_CSTRING> *result= new (table_mem_root) List<LEX_CSTRING>();
  if (!result)
    return true; // OOM

  List_iterator_fast<LEX_CSTRING> columns_it(*src_col_names);
  LEX_CSTRING *column_name;

  while ((column_name= columns_it++))
  {
    LEX_CSTRING *cname= (LEX_CSTRING*)alloc_root(table_mem_root,
                                                 sizeof(LEX_CSTRING));

    if (!cname)
      return true; // OOM

    *cname= safe_lexcstrdup_root(table_mem_root, *column_name);

    if (!cname->str ||
        result->push_back(cname, table_mem_root))
      return true; // OOM
  }

  *dst_col_names= result;
  return false;
}


/**
  Check whenever .TRG file for table exist and load all triggers it contains.

  @param thd          current thread context
  @param db           table's database name
  @param table_name   table's name
  @param table        pointer to table object
  @param names_only   stop after loading trigger names

  @todo
    A lot of things to do here e.g. how about other funcs and being
    more paranoical ?

  @todo
    This could be avoided if there is no triggers for UPDATE and DELETE.

  @retval
    False   no triggers or triggers where correctly loaded
  @retval
    True    error (wrong trigger file)
*/

bool Table_triggers_list::check_n_load(THD *thd, const LEX_CSTRING *db,
                                       const LEX_CSTRING *table_name,
                                       TABLE *table,
                                       bool names_only)
{
  char path_buff[FN_REFLEN];
  LEX_CSTRING path;
  File_parser *parser;
  LEX_CSTRING save_db;
  DBUG_ENTER("Table_triggers_list::check_n_load");

  path.length= build_table_filename(path_buff, FN_REFLEN - 1,
                                    db->str, table_name->str, TRG_EXT, 0);
  path.str= path_buff;

  // QQ: should we analyze errno somehow ?
  if (access(path_buff, F_OK))
    DBUG_RETURN(0);

  /* File exists so we got to load triggers */

  if ((parser= sql_parse_prepare(&path, &table->mem_root, 1)))
  {
    if (is_equal(&triggers_file_type, parser->type()))
    {
      Handle_old_incorrect_sql_modes_hook sql_modes_hook(path.str);
      LEX_CSTRING *trg_create_str;
      ulonglong *trg_sql_mode, *trg_create_time;
      Trigger *trigger;
      Table_triggers_list *trigger_list=
        new (&table->mem_root) Table_triggers_list(table);
      if (unlikely(!trigger_list))
        goto error;

      if (parser->parse((uchar*)trigger_list, &table->mem_root,
                        triggers_file_parameters,
                        TRG_NUM_REQUIRED_PARAMETERS,
                        &sql_modes_hook))
        goto error;

      List_iterator_fast<LEX_CSTRING> it(trigger_list->definitions_list);

      if (!trigger_list->definitions_list.is_empty() &&
          (trigger_list->client_cs_names.is_empty() ||
           trigger_list->connection_cl_names.is_empty() ||
           trigger_list->db_cl_names.is_empty()))
      {
        /* We will later use the current character sets */
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_TRG_NO_CREATION_CTX,
                            ER_THD(thd, ER_TRG_NO_CREATION_CTX),
                            db->str,
                            table_name->str);
      }

      table->triggers= trigger_list;
      status_var_increment(thd->status_var.feature_trigger);

      List_iterator_fast<ulonglong> itm(trigger_list->definition_modes_list);
      List_iterator_fast<LEX_CSTRING> it_definer(trigger_list->definers_list);
      List_iterator_fast<LEX_CSTRING> it_client_cs_name(trigger_list->client_cs_names);
      List_iterator_fast<LEX_CSTRING> it_connection_cl_name(trigger_list->connection_cl_names);
      List_iterator_fast<LEX_CSTRING> it_db_cl_name(trigger_list->db_cl_names);
      List_iterator_fast<ulonglong>
        it_create_times(trigger_list->hr_create_times);
      LEX *old_lex= thd->lex;
      LEX lex;
      sp_rcontext *save_spcont= thd->spcont;
      sql_mode_t save_sql_mode= thd->variables.sql_mode;

      thd->lex= &lex;

      save_db= thd->db;
      thd->reset_db(db);
      while ((trg_create_str= it++))
      {
        sp_head *sp;
        sql_mode_t sql_mode;
        LEX_CSTRING *trg_definer;
        Trigger_creation_ctx *creation_ctx;

        /*
          It is old file format then sql_mode may not be filled in.
          We use one mode (current) for all triggers, because we have not
          information about mode in old format.
        */
        sql_mode= ((trg_sql_mode= itm++) ? *trg_sql_mode :
                   (ulonglong) global_system_variables.sql_mode);

        trg_create_time= it_create_times++;     // May be NULL if old file
        trg_definer= it_definer++;              // May be NULL if old file

        thd->variables.sql_mode= sql_mode;

        Parser_state parser_state;
        if (parser_state.init(thd, (char*) trg_create_str->str,
                              trg_create_str->length))
          goto err_with_lex_cleanup;

        if (!trigger_list->client_cs_names.is_empty())
          creation_ctx= Trigger_creation_ctx::create(thd,
                                                     db->str,
                                                     table_name->str,
                                                     it_client_cs_name++,
                                                     it_connection_cl_name++,
                                                     it_db_cl_name++);
        else
        {
          /* Old file with not stored character sets. Use current */
          creation_ctx=  new
            Trigger_creation_ctx(thd->variables.character_set_client,
                                 thd->variables.collation_connection,
                                 thd->variables.collation_database);
        }

        lex_start(thd);
        thd->spcont= NULL;

        /* The following is for catching parse errors */
        lex.trg_chistics.events= TRG_EVENT_UNKNOWN;
        lex.trg_chistics.action_time= TRG_ACTION_MAX;
        Deprecated_trigger_syntax_handler error_handler;
        thd->push_internal_handler(&error_handler);

        bool parse_error= parse_sql(thd, & parser_state, creation_ctx);
        thd->pop_internal_handler();

        if (parse_error)
        {
          sp_head::destroy(lex.sphead);
          lex.sphead= nullptr;
        }

        /*
          Not strictly necessary to invoke this method here, since we know
          that we've parsed CREATE TRIGGER and not an
          UPDATE/DELETE/INSERT/REPLACE/LOAD/CREATE TABLE, but we try to
          maintain the invariant that this method is called for each
          distinct statement, in case its logic is extended with other
          types of analyses in future.
        */
        lex.set_trg_event_type_for_tables();

        if (lex.sphead)
          lex.sphead->m_sql_mode= sql_mode;

        if (unlikely(!(trigger= (new (&table->mem_root)
                                 Trigger(trigger_list, lex.sphead)))))
          goto err_with_lex_cleanup;
        lex.sphead= NULL; /* Prevent double cleanup. */

        sp= trigger->body;

        trigger->sql_mode= sql_mode;
        trigger->definition= *trg_create_str;
        trigger->hr_create_time.val= trg_create_time ? *trg_create_time : 0;
        /*
          Fix time if in 100th of second (comparison with max uint * 100
          (max possible timestamp in the old format))
        */
        if (trigger->hr_create_time.val < 429496729400ULL)
          trigger->hr_create_time.val*= 10000;
        trigger->name= sp ? Lex_ident_trigger(sp->m_name) :
                            Lex_ident_trigger(empty_clex_str);
        trigger->on_table_name.str= (char*) lex.raw_trg_on_table_name_begin;
        trigger->on_table_name.length= (lex.raw_trg_on_table_name_end -
                                        lex.raw_trg_on_table_name_begin);

        /* Copy pointers to character sets to make trigger easier to use */
        trigger->client_cs_name= creation_ctx->get_client_cs()->cs_name;
        trigger->connection_cl_name= creation_ctx->get_connection_cl()->coll_name;
        trigger->db_cl_name= creation_ctx->get_db_cl()->coll_name;

        if (copy_on_update_columns_list(&table->mem_root,
                                        &trigger->updatable_columns,
                                        lex.trg_chistics.on_update_col_names))
          goto err_with_lex_cleanup;

        /*
          events can be equal TRG_EVENT_UNKNOWN only in case of
          fatal parse errors
        */
        if (lex.trg_chistics.events != TRG_EVENT_UNKNOWN)
        {
          const Lex_ident_trigger
            anchor_trg_name(lex.trg_chistics.anchor_trigger_name);

          trigger_list->add_trigger(lex.trg_chistics.events,
                                    lex.trg_chistics.action_time,
                                    TRG_ORDER_NONE,
                                    anchor_trg_name,
                                    trigger);
        }

        if (unlikely(parse_error))
        {
          const LEX_CSTRING *name;

          /*
            In case of errors, disable all triggers for the table, but keep
            the wrong trigger around to allow the user to fix it
          */
          if (!trigger_list->m_has_unparseable_trigger)
            trigger_list->set_parse_error_message(error_handler.get_error_message());
          /* Currently sphead is always set to NULL in case of a parse error */
          DBUG_ASSERT(lex.sphead == 0);
          lex_end(&lex);

          if (likely((name= error_handler.get_trigger_name())))
          {
            trigger->name= Lex_ident_trigger(safe_lexcstrdup_root(
                                               &table->mem_root, *name));
            if (unlikely(!trigger->name.str))
              goto err_with_lex_cleanup;
          }
          trigger->definer= ((!trg_definer || !trg_definer->length) ?
                             empty_clex_str : *trg_definer);
          continue;
        }

        sp->m_sql_mode= sql_mode;
        sp->set_creation_ctx(creation_ctx);

        if (!trg_definer || !trg_definer->length)
        {
          /*
            This trigger was created/imported from the previous version of
            MySQL, which does not support trigger_list definers. We should emit
            warning here.
          */

          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_TRG_NO_DEFINER,
                              ER_THD(thd, ER_TRG_NO_DEFINER),
                              db->str, sp->m_name.str);

          /*
            Set definer to the '' to correct displaying in the information
            schema.
          */

          sp->set_definer("", 0);
          trigger->definer= empty_clex_str;

          /*
            trigger_list without definer information are executed under the
            authorization of the invoker.
          */

          sp->set_suid(SP_IS_NOT_SUID);
        }
        else
        {
          sp->set_definer(trg_definer->str, trg_definer->length);
          trigger->definer= *trg_definer;
        }

        sp->m_sp_share= MYSQL_GET_SP_SHARE(SP_TYPE_TRIGGER,
                                           sp->m_db.str, static_cast<uint>(sp->m_db.length),
                                           sp->m_name.str, static_cast<uint>(sp->m_name.length));

#ifndef DBUG_OFF
        /*
          Let us check that we correctly update trigger definitions when we
          rename tables with trigger_list.

          In special cases like "RENAME TABLE `#mysql50#somename` TO `somename`"
          or "ALTER DATABASE `#mysql50#somename` UPGRADE DATA DIRECTORY NAME"
          we might be given table or database name with "#mysql50#" prefix (and
          trigger's definiton contains un-prefixed version of the same name).
          To remove this prefix we use check_n_cut_mysql50_prefix().
        */

        char fname[SAFE_NAME_LEN + 1];
        DBUG_ASSERT((lex.query_tables->db.streq(*db) ||
                     (check_n_cut_mysql50_prefix(db->str, fname, sizeof(fname)) &&
                      lex.query_tables->db.streq(Lex_cstring_strlen(fname)))));
        DBUG_ASSERT((lex.query_tables->table_name.streq(*table_name) ||
                     (check_n_cut_mysql50_prefix(table_name->str, fname, sizeof(fname)) &&
                      lex.query_tables->table_name.
                        streq(Lex_cstring_strlen(fname)))));
#endif
        if (names_only)
        {
          lex_end(&lex);
          continue;
        }

        /*
          Also let us bind these objects to Field objects in table being
          opened.

          We ignore errors here, because if even something is wrong we still
          will be willing to open table to perform some operations (e.g.
          SELECT)...
          Anyway some things can be checked only during trigger execution.
        */

        (void)iterate_trigger_fields_and_run_func(
          sp->m_trg_table_fields,
          [thd, table, trigger] (Item_trigger_field* trg_field)
          {
            trg_field->setup_field(thd, table, &trigger->subject_table_grants);
            return false;
          }
        );

        sp->m_trg= trigger;
        lex_end(&lex);
      }
      thd->reset_db(&save_db);
      thd->lex= old_lex;
      thd->spcont= save_spcont;
      thd->variables.sql_mode= save_sql_mode;

      if (!names_only && trigger_list->prepare_record_accessors(table))
        goto error;

      /* Ensure no one is accidently using the temporary load lists */
      trigger_list->empty_lists();
      DBUG_RETURN(0);

err_with_lex_cleanup:
      lex_end(&lex);
      thd->lex= old_lex;
      thd->spcont= save_spcont;
      thd->variables.sql_mode= save_sql_mode;
      thd->reset_db(&save_db);
      /* Fall trough to error */
    }
  }

error:
  if (unlikely(!thd->is_error()))
  {
    /*
      We don't care about this error message much because .TRG files will
      be merged into .FRM anyway.
    */
    my_error(ER_WRONG_OBJECT, MYF(0),
             table_name->str, TRG_EXT + 1, "TRIGGER");
  }
  DBUG_RETURN(1);
}


void Table_triggers_list::add_trigger(trg_event_set trg_events,
                                      trg_action_time_type action_time,
                                      trigger_order_type ordering_clause,
                                      const Lex_ident_trigger &
                                        anchor_trigger_name,
                                      Trigger *trigger)
{
  if (is_trg_event_on(trg_events, TRG_EVENT_INSERT))
    add_trigger(TRG_EVENT_INSERT,
                action_time,
                ordering_clause,
                anchor_trigger_name,
                trigger);

  if (is_trg_event_on(trg_events, TRG_EVENT_UPDATE))
    add_trigger(TRG_EVENT_UPDATE,
                action_time,
                ordering_clause,
                anchor_trigger_name,
                trigger);

  if (is_trg_event_on(trg_events, TRG_EVENT_DELETE))
      add_trigger(TRG_EVENT_DELETE,
                  action_time,
                  ordering_clause,
                  anchor_trigger_name,
                  trigger);
}


/**
   Add trigger in the correct position according to ordering clause
   Also update action order

   If anchor_trigger doesn't exist, add it last.
*/

void Table_triggers_list::add_trigger(trg_event_type event,
                                      trg_action_time_type action_time,
                                      trigger_order_type ordering_clause,
                                      const Lex_ident_trigger &
                                        anchor_trigger_name,
                                      Trigger *trigger)
{
  Trigger **parent= &triggers[event][action_time];
  uint position= 0;

  for ( ; *parent ; parent= &(*parent)->next[event], position++)
  {
    if (ordering_clause != TRG_ORDER_NONE &&
        anchor_trigger_name.streq((*parent)->name))
    {
      if (ordering_clause == TRG_ORDER_FOLLOWS)
      {
        parent= &(*parent)->next[event];        // Add after this one
        position++;
      }
      break;
    }
  }

  /* Add trigger where parent points to */
  trigger->next[event]= *parent;
  *parent= trigger;

  /* Update action_orders and position */
  trigger->events|= trg2bit(event);
  trigger->action_time= action_time;
  trigger->action_order[event]= ++position;
  while ((trigger= trigger->next[event]))
    trigger->action_order[event]= ++position;

  count++;
}


/**
  Obtains and returns trigger metadata.

  @param trigger_stmt  returns statement of trigger
  @param body          returns body of trigger
  @param definer       returns definer/creator of trigger. The caller is
                       responsible to allocate enough space for storing
                       definer information.

  @retval
    False   success
  @retval
    True    error
*/

void Trigger::get_trigger_info(LEX_CSTRING *trigger_stmt,
                               LEX_CSTRING *trigger_body,
                               LEX_STRING *definer)
{
  DBUG_ENTER("get_trigger_info");

  *trigger_stmt= definition;
  if (!body)
  {
    /* Parse error */
    *trigger_body= definition;
    *definer= empty_lex_str;
    DBUG_VOID_RETURN;
  }
  *trigger_body= body->m_body_utf8;

  if (body->suid() == SP_IS_NOT_SUID)
  {
    *definer= empty_lex_str;
  }
  else
  {
    definer->length= strxmov(definer->str, body->m_definer.user.str, "@",
                             body->m_definer.host.str, NullS) - definer->str;
  }
  DBUG_VOID_RETURN;
}


/**
  Find trigger's table from trigger identifier and add it to
  the statement table list.

  @param[in] thd       Thread context.
  @param[in] trg_name  Trigger name.
  @param[in] if_exists TRUE if SQL statement contains "IF EXISTS" clause.
                       That means a warning instead of error should be
                       thrown if trigger with given name does not exist.
  @param[out] table    Pointer to TABLE_LIST object for the
                       table trigger.

  @return Operation status
    @retval FALSE On success.
    @retval TRUE  Otherwise.
*/

static bool add_table_for_trigger_internal(THD *thd,
                                           const sp_name *trg_name,
                                           bool if_exists,
                                           TABLE_LIST **table,
                                           char *trn_path_buff)
{
  LEX *lex= thd->lex;
  LEX_CSTRING trn_path= { trn_path_buff, 0 };
  LEX_CSTRING tbl_name= null_clex_str;
  DBUG_ENTER("add_table_for_trigger_internal");

  build_trn_path(thd, trg_name, (LEX_STRING*) &trn_path);

  if (check_trn_exists(&trn_path))
  {
    if (if_exists)
    {
      push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                   ER_TRG_DOES_NOT_EXIST, ER_THD(thd, ER_TRG_DOES_NOT_EXIST));

      *table= NULL;

      DBUG_RETURN(FALSE);
    }

    my_error(ER_TRG_DOES_NOT_EXIST, MYF(0));
    DBUG_RETURN(TRUE);
  }

  if (load_table_name_for_trigger(thd, trg_name, &trn_path, &tbl_name))
    DBUG_RETURN(TRUE);

  *table= sp_add_to_query_tables(thd, lex, &trg_name->m_db,
                                 &tbl_name, TL_IGNORE,
                                 MDL_SHARED_NO_WRITE);

  DBUG_RETURN(*table ? FALSE : TRUE);
}


/*
  Same as above, but with an allocated buffer.
  This is called by mysql_excute_command() in is here to keep stack
  space down in the caller.
*/

bool add_table_for_trigger(THD *thd,
                           const sp_name *trg_name,
                           bool if_exists,
                           TABLE_LIST **table)
{
  char trn_path_buff[FN_REFLEN];
  return add_table_for_trigger_internal(thd, trg_name, if_exists,
                                        table, trn_path_buff);
}


/**
  Drop all triggers for table.

  @param thd      current thread context
  @param db       schema for table
  @param name     name for table

  @retval
    False   success
  @retval
    True    error
*/

bool Table_triggers_list::drop_all_triggers(THD *thd, const LEX_CSTRING *db,
                                            const LEX_CSTRING *name,
                                            myf MyFlags)
{
  TABLE table;
  char path[FN_REFLEN];
  bool result= 0;
  DBUG_ENTER("Table_triggers_list::drop_all_triggers");

  table.reset();
  init_sql_alloc(key_memory_Table_trigger_dispatcher,
                 &table.mem_root, 8192, 0, MYF(MY_WME));

  if (Table_triggers_list::check_n_load(thd, db, name, &table, 1))
  {
    result= 1;
    /* We couldn't parse trigger file, best to just remove it */
    rm_trigger_file(path, db, name, MyFlags);
    goto end;
  }
  if (table.triggers)
  {
    for (uint i= 0; i < (uint)TRG_EVENT_MAX; i++)
    {
      for (uint j= 0; j < (uint)TRG_ACTION_MAX; j++)
      {
        Trigger *trigger;
        for (trigger= table.triggers->get_trigger(i,j) ;
             trigger ;
             trigger= trigger->next[i])
        {
          /*
            Trigger, which body we failed to parse during call
            Table_triggers_list::check_n_load(), might be missing name.
            Such triggers have zero-length name and are skipped here.
          */
          if (trigger->name.length &&
              rm_trigname_file(path, db, &trigger->name, MyFlags))
          {
            /*
              Instead of immediately bailing out with error if we were unable
              to remove .TRN file we will try to drop other files.
            */
            result= 1;
          }
          /* Drop statistics for this stored program from performance schema. */
          MYSQL_DROP_SP(SP_TYPE_TRIGGER, db->str, static_cast<uint>(db->length),
                        trigger->name.str, static_cast<uint>(trigger->name.length));
        }
      }
    }
    if (rm_trigger_file(path, db, name, MyFlags))
      result= 1;
    delete table.triggers;
  }
end:
  free_root(&table.mem_root, MYF(0));
  DBUG_RETURN(result);
}


/**
  Update .TRG file after renaming triggers' subject table
  (change name of table in triggers' definitions).

  @param thd                 Thread context
  @param old_db_name         Old database of subject table
  @param new_db_name         New database of subject table
  @param old_table_name      Old subject table's name
  @param new_table_name      New subject table's name

  @retval
    FALSE  Success
  @retval
    TRUE   Failure
*/

struct change_table_name_param
{
  THD *thd;
  LEX_CSTRING *old_db_name;
  LEX_CSTRING *new_db_name;
  LEX_CSTRING *new_table_name;
  Trigger *stopper;
};


bool
Table_triggers_list::
change_table_name_in_triggers(THD *thd,
                              const LEX_CSTRING *old_db_name,
                              const LEX_CSTRING *new_db_name,
                              const LEX_CSTRING *old_table_name,
                              const LEX_CSTRING *new_table_name)
{
  struct change_table_name_param param;
  sql_mode_t save_sql_mode= thd->variables.sql_mode;
  char path_buff[FN_REFLEN];

  param.thd= thd;
  param.new_table_name= const_cast<LEX_CSTRING*>(new_table_name);

  for_all_triggers(&Trigger::change_table_name, &param);

  thd->variables.sql_mode= save_sql_mode;

  if (unlikely(thd->is_fatal_error))
    return TRUE; /* OOM */

  if (save_trigger_file(thd, new_db_name, new_table_name))
    return TRUE;

  if (rm_trigger_file(path_buff, old_db_name, old_table_name, MYF(MY_WME)))
  {
    (void) rm_trigger_file(path_buff, new_db_name, new_table_name,
                           MYF(MY_WME));
    return TRUE;
  }
  return FALSE;
}


bool Trigger::change_table_name(void* param_arg)
{
  change_table_name_param *param= (change_table_name_param*) param_arg;
  THD *thd= param->thd;
  LEX_CSTRING *new_table_name= param->new_table_name;
  LEX_CSTRING *def= &definition, new_def;
  size_t on_q_table_name_len, before_on_len;
  String buff;

  thd->variables.sql_mode= sql_mode;

  /* Construct CREATE TRIGGER statement with new table name. */
  buff.length(0);

  /* WARNING: 'on_table_name' is supposed to point inside 'def' */
  DBUG_ASSERT(on_table_name.str > def->str);
  DBUG_ASSERT(on_table_name.str < (def->str + def->length));
  before_on_len= on_table_name.str - def->str;

  buff.append(def->str, before_on_len);
  buff.append(STRING_WITH_LEN("ON "));
  append_identifier(thd, &buff, new_table_name);
  buff.append(STRING_WITH_LEN(" "));
  on_q_table_name_len= buff.length() - before_on_len;
  buff.append(on_table_name.str + on_table_name.length,
              def->length - (before_on_len + on_table_name.length));
  /*
    It is OK to allocate some memory on table's MEM_ROOT since this
    table instance will be thrown out at the end of rename anyway.
  */
  new_def.str= (char*) memdup_root(&base->trigger_table->mem_root, buff.ptr(),
                                   buff.length());
  new_def.length= buff.length();
  on_table_name.str= new_def.str + before_on_len;
  on_table_name.length= on_q_table_name_len;
  definition= new_def;
  return 0;
}


/**
  Iterate though Table_triggers_list::names_list list and update
  .TRN files after renaming triggers' subject table.

  @param old_db_name         Old database of subject table
  @param new_db_name         New database of subject table
  @param new_table_name      New subject table's name
  @param stopper             Pointer to Table_triggers_list::names_list at
                             which we should stop updating.

  @retval
    0      Success
  @retval
    non-0  Failure, pointer to Table_triggers_list::names_list element
    for which update failed.
*/

Trigger *
Table_triggers_list::
change_table_name_in_trignames(const LEX_CSTRING *old_db_name,
                               const LEX_CSTRING *new_db_name,
                               const LEX_CSTRING *new_table_name,
                               Trigger *trigger)
{
  struct change_table_name_param param;
  param.old_db_name=    const_cast<LEX_CSTRING*>(old_db_name);
  param.new_db_name=    const_cast<LEX_CSTRING*>(new_db_name);
  param.new_table_name= const_cast<LEX_CSTRING*>(new_table_name);
  param.stopper= trigger;

  return for_all_triggers(&Trigger::change_on_table_name, &param);
}


bool Trigger::change_on_table_name(void* param_arg)
{
  change_table_name_param *param= (change_table_name_param*) param_arg;

  char trigname_buff[FN_REFLEN];
  struct st_trigname trigname;
  LEX_CSTRING trigname_file;

  if (param->stopper == this)
    return 0;                                   // Stop processing

  trigname_file.length= build_table_filename(trigname_buff, FN_REFLEN-1,
                                             param->new_db_name->str, name.str,
                                             TRN_EXT, 0);
  trigname_file.str= trigname_buff;

  trigname.trigger_table= *param->new_table_name;

  if (base->create_lists_needed_for_files(current_thd->mem_root))
    return true;

  if (sql_create_definition_file(NULL, &trigname_file, &trigname_file_type,
                                 (uchar*)&trigname, trigname_file_parameters))
    return true;

  /* Remove stale .TRN file in case of database upgrade */
  if (param->old_db_name)
  {
    if (rm_trigname_file(trigname_buff, param->old_db_name, &name,
                         MYF(MY_WME)))
    {
      (void) rm_trigname_file(trigname_buff, param->new_db_name, &name,
                              MYF(MY_WME));
      return 1;
    }
  }
  return 0;
}


/*
  Check if we can rename triggers in change_table_name()
  The idea is to ensure that it is close to impossible that
  change_table_name() should fail.

  @return 0 ok
  @return 1 Error: rename of triggers would fail
*/

bool
Table_triggers_list::prepare_for_rename(THD *thd,
                                        TRIGGER_RENAME_PARAM *param,
                                        const Lex_ident_db &db,
                                        const Lex_ident_table &old_alias,
                                        const Lex_ident_table &old_table,
                                        const Lex_ident_db &new_db,
                                        const Lex_ident_table &new_table)
{
  TABLE *table= &param->table;
  bool result= 0;
  DBUG_ENTER("Table_triggers_lists::prepare_change_table_name");

  init_sql_alloc(key_memory_Table_trigger_dispatcher,
                 &table->mem_root, 8192, 0, MYF(0));

  DBUG_ASSERT(!db.streq(new_db) ||
              !old_alias.streq(new_table));

  if (Table_triggers_list::check_n_load(thd, &db, &old_table, table, TRUE))
  {
    result= 1;
    goto end;
  }
  if (table->triggers)
  {
    if (table->triggers->check_for_broken_triggers())
    {
      result= 1;
      goto end;
    }
    /*
      Since triggers should be in the same schema as their subject tables
      moving table with them between two schemas raises too many questions.
      (E.g. what should happen if in new schema we already have trigger
       with same name ?).

      In case of "ALTER DATABASE `#mysql50#db1` UPGRADE DATA DIRECTORY NAME"
      we will be given table name with "#mysql50#" prefix
      To remove this prefix we use check_n_cut_mysql50_prefix().
    */
    if (!db.streq(new_db))
    {
      char dbname[SAFE_NAME_LEN + 1];
      if (check_n_cut_mysql50_prefix(db.str, dbname, sizeof(dbname)) &&
          new_db.streq(Lex_cstring_strlen(dbname)))
      {
        param->upgrading50to51= TRUE;
      }
      else
      {
        my_error(ER_TRG_IN_WRONG_SCHEMA, MYF(0));
        result= 1;
        goto end;
      }
    }
  }

end:
  param->got_error= result;
  DBUG_RETURN(result);
}


/**
  Update .TRG and .TRN files after renaming triggers' subject table.

  @param[in,out] thd Thread context
  @param[in] db Old database of subject table
  @param[in] old_alias Old alias of subject table
  @param[in] old_table Old name of subject table. The difference between
             old_table and old_alias is that in case of lower_case_table_names
             old_table == lowercase(old_alias)
  @param[in] new_db New database for subject table
  @param[in] new_table New name of subject table

  @note
    This method tries to leave trigger related files in consistent state,
    i.e. it either will complete successfully, or will fail leaving files
    in their initial state.
    Also this method assumes that subject table is not renamed to itself.
    This method needs to be called under an exclusive table metadata lock.

  @retval FALSE Success
  @retval TRUE  Error
*/

bool Table_triggers_list::change_table_name(THD *thd,
                                            TRIGGER_RENAME_PARAM *param,
                                            const LEX_CSTRING *db,
                                            const LEX_CSTRING *old_alias,
                                            const LEX_CSTRING *old_table,
                                            const LEX_CSTRING *new_db,
                                            const LEX_CSTRING *new_table)
{
  TABLE *table= &param->table;
  bool result= 0;
  bool upgrading50to51= FALSE;
  Trigger *err_trigger;
  DBUG_ENTER("Table_triggers_list::change_table_name");

  DBUG_ASSERT(!param->got_error);
  /*
    This method interfaces the mysql server code protected by
    an exclusive metadata lock.
  */
  DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, db->str,
                                             old_table->str,
                                             MDL_EXCLUSIVE));

  if (table->triggers)
  {
    if (unlikely(table->triggers->change_table_name_in_triggers(thd, db, new_db,
                                                               old_alias,
                                                               new_table)))
    {
      result= 1;
      goto end;
    }
    if ((err_trigger= table->triggers->
         change_table_name_in_trignames( upgrading50to51 ? db : NULL,
                                         new_db, new_table, 0)))
    {
      /*
        If we were unable to update one of .TRN files properly we will
        revert all changes that we have done and report about error.
        We assume that we will be able to undo our changes without errors
        (we can't do much if there will be an error anyway).
      */
      (void) table->triggers->change_table_name_in_trignames(
                               upgrading50to51 ? new_db : NULL, db,
                               old_alias, err_trigger);
      (void) table->triggers->change_table_name_in_triggers(
                               thd, db, new_db,
                               new_table, old_alias);
      result= 1;
      goto end;
    }
  }

end:
  DBUG_RETURN(result);
}


/**
  Check that a BEFORE trigger has raised the signal to inform that
  a current row being processed must be skipped.

  @param      da                  Diagnostics area
  @param[out] skip_row_indicator  where to store the fact about skipping
                                  the row
  @param      time_type           time when trigger is invoked (i.e. before or
                                  after)

  @return true in case the current row must be skipped, else false
*/

static inline bool do_skip_row_indicator(Diagnostics_area *da,
                                         bool *skip_row_indicator,
                                         trg_action_time_type time_type)
{
  if (!skip_row_indicator)
    return false;

  if (time_type == TRG_ACTION_BEFORE &&
      /*
        The '02' class signals a 'no data' condition, the subclass '02TRG'
        means 'no data in trigger' and this condition shouldn't be treated
        as an error.
      */
      strcmp(da->get_sqlstate(), "02TRG") == 0)
  {
    *skip_row_indicator= true;
    return true;
  }
  return false;
}


/**
  This class is responsible for storing a kind of current trigger event
  for processing of NEW/OLD clauses inside trigger's body.
  Before start processing of triggers for the given event type, the event type
  pushed into the stack of events in constructor of the class
  Trigger_event_guard and popped after processing all triggers of this event
  type by running destructor of the class Trigger_event_guard.

  Every time when the NEW or OLD clause is evaluated on processing a trigger
  body, the event type of trigger being executed is consulted to determine
  whether a value of the clause can produce meaning value: for INSERT event,
  evaluation of the OLD clause should return NULL; for DELETE event, evaluation
  of the NEW clause should return NULL.
  @see Item_trigger_field::check_new_old_qulifiers_comform_with_trg_event()
  @see Item_trigger_field::save_in_field()
  @see Item_trigger_field::val_*()
*/
class Trigger_event_guard
{
  Statement *m_stmt;
public:
  Trigger_event_guard(Statement *stmt,
                      trg_event_type event)
  : m_stmt{stmt}
  {
    m_stmt->push_current_trg_event(event);
  }
  ~Trigger_event_guard()
  {
    m_stmt->pop_current_trg_event();
  }
};


/**
  Execute trigger for given (event, time) pair.

  The operation executes trigger for the specified event (insert, update,
  delete) and time (after, before) if it is set.

  @param thd
  @param event
  @param time_type
  @param old_row_is_record1
  @param[out] skip_row_indicator  the flag to tell whether a row must be
                                  skipped by the INSERT statement

  @return Error status.
    @retval FALSE on success.
    @retval TRUE  on error.
*/

bool Table_triggers_list::process_triggers(THD *thd,
                                           trg_event_type event,
                                           trg_action_time_type time_type,
                                           bool old_row_is_record1,
                                           bool *skip_row_indicator,
                                           List<Item> *fields_in_update_stmt)
{
  bool err_status;
  Sub_statement_state statement_state;
  Trigger *trigger;
  SELECT_LEX *save_current_select;

  /*
    skip_row_indicator != nullptr for BEFORE INSERT/UPDATE/DELETE triggers
  */
  DBUG_ASSERT((time_type == TRG_ACTION_BEFORE && skip_row_indicator) ||
              (time_type == TRG_ACTION_AFTER && !skip_row_indicator));
  /*
    In case skip_indicator points to an out variable, its initial value
    must be false
  */
  DBUG_ASSERT(!skip_row_indicator ||
              (skip_row_indicator && *skip_row_indicator == false));

  if (check_for_broken_triggers())
    return TRUE;

  if (!(trigger= get_trigger(event, time_type)))
    return FALSE;

  if (old_row_is_record1)
  {
    old_field= record1_field;
    new_field= record0_field;
  }
  else
  {
    DBUG_ASSERT(event == TRG_EVENT_DELETE);
    new_field= record1_field;
    old_field= record0_field;
  }
  /*
    This trigger must have been processed by the pre-locking
    algorithm.
  */
  DBUG_ASSERT(trigger_table->pos_in_table_list->trg_event_map & trg2bit(event));

  if (time_type == TRG_ACTION_AFTER)
    thd->reset_sub_statement_state(&statement_state, SUB_STMT_TRIGGER);
  else
    /*
      For time type TRG_ACTION_BEFORE, set extra flag SUB_STMT_BEFORE_TRIGGER
      at sub statement state in addition to SUB_STMT_TRIGGER in order to
      be able to reset the m_sql_errno to the value
        ER_SIGNAL_SKIP_ROW_FROM_TRIGGER
      in case signal is raised with SQLSTATE "02TRG" from within a
      BEFORE trigger and don't modify m_sql_errno in case the signal is raised
      from AFTER trigger.
      @see Sql_state_errno_level::assign_defaults
    */
    thd->reset_sub_statement_state(&statement_state,
                                   SUB_STMT_TRIGGER | SUB_STMT_BEFORE_TRIGGER);
  /*
    Reset current_select before call execute_trigger() and
    restore it after return from one. This way error is set
    in case of failure during trigger execution.
  */
  save_current_select= thd->lex->current_select;

  /*
    Reset the sentinel thd->bulk_param in order not to consume the next
    values of a bound array in case one of statement executed by
    the trigger's body is a DML statement.
  */
  void *save_bulk_param= thd->bulk_param;
  thd->bulk_param= nullptr;

  Trigger_event_guard guard(thd, event);
  do {
    thd->lex->current_select= NULL;

    /*
      For BEFORE UPDATE trigger check that table fields specified by
      the UPDATE statement matches with column names defined in FOR UPDATE
      trigger definition, if any.
    */
    if (event == TRG_EVENT_UPDATE &&
        fields_in_update_stmt &&
        !trigger->match_updatable_columns(*fields_in_update_stmt))
    {
      err_status= 0;
      continue;
    }

    err_status=
      trigger->body->execute_trigger(thd,
                                     &trigger_table->s->db,
                                     &trigger_table->s->table_name,
                                     &trigger->subject_table_grants);

    if (err_status &&
        do_skip_row_indicator(thd->get_stmt_da(), skip_row_indicator,
                              time_type))
    {
      /* Reset DA that is set on handling the statement
           SIGNAL SSQLSTATE "02TRG"
         raised from within a trigger */
      err_status= false;
      thd->get_stmt_da()->reset_diagnostics_area();
    }

    status_var_increment(thd->status_var.executed_triggers);
  } while (!err_status && (trigger= trigger->next[event]));
  thd->bulk_param= save_bulk_param;
  thd->lex->current_select= save_current_select;

  thd->restore_sub_statement_state(&statement_state);

  return err_status;
}


/**
  Add triggers for table to the set of routines used by statement.
  Add tables used by them to statement table list. Do the same for
  routines used by triggers.

  @param thd             Thread context.
  @param prelocking_ctx  Prelocking context of the statement.
  @param table_list      Table list element for table with trigger.

  @retval FALSE  Success.
  @retval TRUE   Failure.
*/

bool
Table_triggers_list::
add_tables_and_routines_for_triggers(THD *thd,
                                     Query_tables_list *prelocking_ctx,
                                     TABLE_LIST *table_list)
{
  DBUG_ASSERT(static_cast<int>(table_list->lock_type) >=
              static_cast<int>(TL_FIRST_WRITE));

  for (int i= 0; i < (int)TRG_EVENT_MAX; i++)
  {
    if (table_list->trg_event_map & trg2bit(static_cast<trg_event_type>(i)))
    {
      for (int j= 0; j < (int)TRG_ACTION_MAX; j++)
      {
        Trigger *triggers= table_list->table->triggers->get_trigger(i,j);

        for ( ; triggers ; triggers= triggers->next[i])
        {
          sp_head *trigger= triggers->body;

          if (unlikely(!triggers->body))                  // Parse error
            continue;

          MDL_key key(MDL_key::TRIGGER, trigger->m_db.str, trigger->m_name.str);

          if (sp_add_used_routine(prelocking_ctx,
                                  thd->active_stmt_arena_to_use(),
                                  &key, &sp_handler_trigger,
                                  table_list->belong_to_view))
          {
            trigger->add_used_tables_to_table_list(thd,
                       &prelocking_ctx->query_tables_last,
                       table_list->belong_to_view);
            sp_update_stmt_used_routines(thd, prelocking_ctx,
                                         &trigger->m_sroutines,
                                         table_list->belong_to_view);
            trigger->propagate_attributes(prelocking_ctx);
          }
        }
      }
    }
  }
  return FALSE;
}


/**
  Check whether there is an ON UPDATE trigger that modifies any of fields
  supplied by the UPDATE statement.

  @param fields  A list of table's fields being modified by
                 the UPDATE statement

  @return true in case there is a trigger that modifies any of the fields
               passed in the UPDATE statement, else false
 */
bool Table_triggers_list::match_updatable_columns(List<Item> *fields)
{
  for (uint i= 0; i < (uint)TRG_ACTION_MAX; i++)
  {
    for (Trigger *trigger= get_trigger(TRG_EVENT_UPDATE, i) ;
         trigger ;
         trigger= trigger->next[TRG_EVENT_UPDATE])
      if (trigger->match_updatable_columns(*fields))
        return true;
  }

  return false;
}


/**
  Mark fields of subject table which we read/set in its triggers
  as such.

  This method marks fields of subject table which are read/set in its
  triggers as such (by properly updating TABLE::read_set/write_set)
  and thus informs handler that values for these fields should be
  retrieved/stored during execution of statement.

  @param thd    Current thread context
  @param event  Type of event triggers for which we are going to inspect
*/

void Table_triggers_list::mark_fields_used(trg_event_type event)
{
  int action_time;
  DBUG_ENTER("Table_triggers_list::mark_fields_used");

  for (action_time= 0; action_time < (int)TRG_ACTION_MAX; action_time++)
  {
    for (Trigger *trigger= get_trigger(event,action_time);
         trigger ;
         trigger= trigger->next[event])
    {
      /*
        Skip a trigger that was parsed with an error.
      */
      if (trigger->body == nullptr)
        continue;

      (void)iterate_trigger_fields_and_run_func(
        trigger->body->m_trg_table_fields,
        [this] (Item_trigger_field* trg_field)
        {
          /* We cannot mark fields which does not present in table. */
          if (trg_field->field_idx != NO_CACHED_FIELD_INDEX)
          {
            DBUG_PRINT("info", ("marking field: %u", (uint) trg_field->field_idx));
            if (trg_field->get_settable_routine_parameter())
              bitmap_set_bit(trigger_table->write_set, trg_field->field_idx);
            trigger_table->mark_column_with_deps(
              trigger_table->field[trg_field->field_idx]);
          }
          return false;
        }
      );
    }
  }
  trigger_table->file->column_bitmaps_signal();
  DBUG_VOID_RETURN;
}


/**
   Signals to the Table_triggers_list that a parse error has occurred when
   reading a trigger from file. This makes the Table_triggers_list enter an
   error state flagged by m_has_unparseable_trigger == true. The error message
   will be used whenever a statement invoking or manipulating triggers is
   issued against the Table_triggers_list's table.

   @param error_message The error message thrown by the parser.
 */
void Table_triggers_list::set_parse_error_message(char *error_message)
{
  m_has_unparseable_trigger= true;
  strnmov(m_parse_error_message, error_message,
          sizeof(m_parse_error_message)-1);
}


/**
  Trigger BUG#14090 compatibility hook.

  @param[in,out] unknown_key       reference on the line with unknown
    parameter and the parsing point
  @param[in]     base              base address for parameter writing
    (structure like TABLE)
  @param[in]     mem_root          MEM_ROOT for parameters allocation
  @param[in]     end               the end of the configuration

  @note
    NOTE: this hook process back compatibility for incorrectly written
    sql_modes parameter (see BUG#14090).

  @retval
    FALSE OK
  @retval
    TRUE  Error
*/

#define INVALID_SQL_MODES_LENGTH 13

bool
Handle_old_incorrect_sql_modes_hook::
process_unknown_string(const char *&unknown_key, uchar* base,
                       MEM_ROOT *mem_root, const char *end)
{
  DBUG_ENTER("Handle_old_incorrect_sql_modes_hook::process_unknown_string");
  DBUG_PRINT("info", ("unknown key: %60s", unknown_key));

  if (unknown_key + INVALID_SQL_MODES_LENGTH + 1 < end &&
      unknown_key[INVALID_SQL_MODES_LENGTH] == '=' &&
      !memcmp(unknown_key, STRING_WITH_LEN("sql_modes")))
  {
    THD *thd= current_thd;
    const char *ptr= unknown_key + INVALID_SQL_MODES_LENGTH + 1;

    DBUG_PRINT("info", ("sql_modes affected by BUG#14090 detected"));
    push_warning_printf(thd,
                        Sql_condition::WARN_LEVEL_NOTE,
                        ER_OLD_FILE_FORMAT,
                        ER_THD(thd, ER_OLD_FILE_FORMAT),
                        (char *)path, "TRIGGER");
    if (get_file_options_ulllist(ptr, end, unknown_key, base,
                                 &sql_modes_parameters, mem_root))
    {
      DBUG_RETURN(TRUE);
    }
    /*
      Set parsing pointer to the last symbol of string (\n)
      1) to avoid problem with \0 in the junk after sql_modes
      2) to speed up skipping this line by parser.
    */
    unknown_key= ptr-1;
  }
  DBUG_RETURN(FALSE);
}

#define INVALID_TRIGGER_TABLE_LENGTH 15

/**
  Trigger BUG#15921 compatibility hook. For details see
  Handle_old_incorrect_sql_modes_hook::process_unknown_string().
*/
bool
Handle_old_incorrect_trigger_table_hook::
process_unknown_string(const char *&unknown_key, uchar* base,
                       MEM_ROOT *mem_root, const char *end)
{
  DBUG_ENTER("Handle_old_incorrect_trigger_table_hook::process_unknown_string");
  DBUG_PRINT("info", ("unknown key: %60s", unknown_key));

  if (unknown_key + INVALID_TRIGGER_TABLE_LENGTH + 1 < end &&
      unknown_key[INVALID_TRIGGER_TABLE_LENGTH] == '=' &&
      !memcmp(unknown_key, STRING_WITH_LEN("trigger_table")))
  {
    THD *thd= current_thd;
    const char *ptr= unknown_key + INVALID_TRIGGER_TABLE_LENGTH + 1;

    DBUG_PRINT("info", ("trigger_table affected by BUG#15921 detected"));
    push_warning_printf(thd,
                        Sql_condition::WARN_LEVEL_NOTE,
                        ER_OLD_FILE_FORMAT,
                        ER_THD(thd, ER_OLD_FILE_FORMAT),
                        (char *)path, "TRIGGER");

    if (!(ptr= parse_escaped_string(ptr, end, mem_root, trigger_table_value)))
    {
      my_error(ER_FPARSER_ERROR_IN_PARAMETER, MYF(0), "trigger_table",
               unknown_key);
      DBUG_RETURN(TRUE);
    }

    /* Set parsing pointer to the last symbol of string (\n). */
    unknown_key= ptr-1;
  }
  DBUG_RETURN(FALSE);
}


/**
  Construct path to TRN-file.

  @param thd[in]        Thread context.
  @param trg_name[in]   Trigger name.
  @param trn_path[out]  Variable to store constructed path
*/

void build_trn_path(THD *thd, const sp_name *trg_name, LEX_STRING *trn_path)
{
  /* Construct path to the TRN-file. */

  trn_path->length= build_table_filename(trn_path->str,
                                         FN_REFLEN - 1,
                                         trg_name->m_db.str,
                                         trg_name->m_name.str,
                                         TRN_EXT,
                                         0);
}


/**
  Check if TRN-file exists.

  @return
    @retval TRUE  if TRN-file does not exist.
    @retval FALSE if TRN-file exists.
*/

bool check_trn_exists(const LEX_CSTRING *trn_path)
{
  return access(trn_path->str, F_OK) != 0;
}


/**
  Retrieve table name for given trigger.

  @param thd[in]        Thread context.
  @param trg_name[in]   Trigger name.
  @param trn_path[in]   Path to the corresponding TRN-file.
  @param tbl_name[out]  Variable to store retrieved table name.

  @return Error status.
    @retval FALSE on success.
    @retval TRUE  if table name could not be retrieved.
*/

bool load_table_name_for_trigger(THD *thd,
                                 const sp_name *trg_name,
                                 const LEX_CSTRING *trn_path,
                                 LEX_CSTRING *tbl_name)
{
  File_parser *parser;
  struct st_trigname trn_data;
  Handle_old_incorrect_trigger_table_hook trigger_table_hook(
                                          trn_path->str,
                                          &trn_data.trigger_table);
  DBUG_ENTER("load_table_name_for_trigger");

  /* Parse the TRN-file. */

  if (!(parser= sql_parse_prepare(trn_path, thd->mem_root, TRUE)))
    DBUG_RETURN(TRUE);

  if (!is_equal(&trigname_file_type, parser->type()))
  {
    my_error(ER_WRONG_OBJECT, MYF(0),
             trg_name->m_name.str,
             TRN_EXT + 1,
             "TRIGGERNAME");

    DBUG_RETURN(TRUE);
  }

  if (parser->parse((uchar*) &trn_data, thd->mem_root,
                    trigname_file_parameters, 1,
                    &trigger_table_hook))
    DBUG_RETURN(TRUE);

  /* Copy trigger table name. */

  *tbl_name= trn_data.trigger_table;

  /* That's all. */

  DBUG_RETURN(FALSE);
}
