/******************************************************
Smart ALTER TABLE

(c) 2005-2007 Innobase Oy
*******************************************************/

#include <mysql_priv.h>
#include <mysqld_error.h>

extern "C" {
#include "log0log.h"
#include "row0merge.h"
#include "srv0srv.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "ha_prototypes.h"
#include "handler0alter.h"
}

#include "ha_innodb.h"

/*****************************************************************
Copies an InnoDB column to a MySQL field.  This function is
adapted from row_sel_field_store_in_mysql_format(). */
static
void
innobase_col_to_mysql(
/*==================*/
	const dict_col_t*	col,	/* in: InnoDB column */
	const uchar*		data,	/* in: InnoDB column data */
	ulint			len,	/* in: length of data, in bytes */
	Field*			field)	/* in/out: MySQL field */
{
	uchar*	ptr;
	uchar*	dest	= field->ptr;
	ulint	flen	= field->pack_length();

	switch (col->mtype) {
	case DATA_INT:
		ut_ad(len == flen);

		/* Convert integer data from Innobase to little-endian
		format, sign bit restored to normal */

		for (ptr = dest + len; ptr != dest; ) {
			*--ptr = *data++;
		}

		if (!(field->flags & UNSIGNED_FLAG)) {
			((byte*) dest)[len - 1] ^= 0x80;
		}

		break;

	case DATA_VARCHAR:
	case DATA_VARMYSQL:
	case DATA_BINARY:
		field->reset();

		if (field->type() == MYSQL_TYPE_VARCHAR) {
			/* This is a >= 5.0.3 type true VARCHAR. Store the
			length of the data to the first byte or the first
			two bytes of dest. */

			dest = row_mysql_store_true_var_len(
				dest, len, flen - field->key_length());
		}

		/* Copy the actual data */
		memcpy(dest, data, len);
		break;

	case DATA_BLOB:
		/* Store a pointer to the BLOB buffer to dest: the BLOB was
		already copied to the buffer in row_sel_store_mysql_rec */

		row_mysql_store_blob_ref(dest, flen, data, len);
		break;

#ifdef UNIV_DEBUG
	case DATA_MYSQL:
		ut_ad(flen >= len);
		ut_ad(col->mbmaxlen >= col->mbminlen);
		ut_ad(col->mbmaxlen > col->mbminlen || flen == len);
		memcpy(dest, data, len);
		break;

	default:
	case DATA_SYS_CHILD:
	case DATA_SYS:
		/* These column types should never be shipped to MySQL. */
		ut_ad(0);

	case DATA_CHAR:
	case DATA_FIXBINARY:
	case DATA_FLOAT:
	case DATA_DOUBLE:
	case DATA_DECIMAL:
		/* Above are the valid column types for MySQL data. */
		ut_ad(flen == len);
#else /* UNIV_DEBUG */
	default:
#endif /* UNIV_DEBUG */
		memcpy(dest, data, len);
	}
}

/*****************************************************************
Copies an InnoDB record to table->record[0]. */
extern "C"
void
innobase_rec_to_mysql(
/*==================*/
	TABLE*			table,		/* in/out: MySQL table */
	const rec_t*		rec,		/* in: record */
	const dict_index_t*	index,		/* in: index */
	const ulint*		offsets)	/* in: rec_get_offsets(
						rec, index, ...) */
{
	uint	n_fields	= table->s->fields;
	uint	i;

	ut_ad(n_fields == dict_table_get_n_user_cols(index->table));

	for (i = 0; i < n_fields; i++) {
		Field*		field	= table->field[i];
		ulint		ipos;
		ulint		ilen;
		const uchar*	ifield;

		field->reset();

		ipos = dict_index_get_nth_col_pos(index, i);

		if (UNIV_UNLIKELY(ipos == ULINT_UNDEFINED)) {
null_field:
			field->set_null();
			continue;
		}

		ifield = rec_get_nth_field(rec, offsets, ipos, &ilen);

		/* Assign the NULL flag */
		if (ilen == UNIV_SQL_NULL) {
			ut_ad(field->real_maybe_null());
			goto null_field;
		}

		field->set_notnull();

		innobase_col_to_mysql(
			dict_field_get_col(
				dict_index_get_nth_field(index, ipos)),
			ifield, ilen, field);
	}
}

/*****************************************************************
Resets table->record[0]. */
extern "C"
void
innobase_rec_reset(
/*===============*/
	TABLE*			table)		/* in/out: MySQL table */
{
	uint	n_fields	= table->s->fields;
	uint	i;

	for (i = 0; i < n_fields; i++) {
		table->field[i]->set_default();
	}
}

/**********************************************************************
Removes the filename encoding of a database and table name. */
static
void
innobase_convert_tablename(
/*=======================*/
	char*	s)	/* in: identifier; out: decoded identifier */
{
	uint	errors;

	char*	slash = strchr(s, '/');

	if (slash) {
		char*	t;
		/* Temporarily replace the '/' with NUL. */
		*slash = 0;
		/* Convert the database name. */
		strconvert(&my_charset_filename, s, system_charset_info,
			   s, slash - s + 1, &errors);

		t = s + strlen(s);
		ut_ad(slash >= t);
		/* Append a  '.' after the database name. */
		*t++ = '.';
		slash++;
		/* Convert the table name. */
		strconvert(&my_charset_filename, slash, system_charset_info,
			   t, slash - t + strlen(slash), &errors);
	} else {
		strconvert(&my_charset_filename, s,
			   system_charset_info, s, strlen(s), &errors);
	}
}

/***********************************************************************
This function checks that index keys are sensible. */
static
int
innobase_check_index_keys(
/*======================*/
					/* out: 0 or error number */
	TABLE*		table,		/* in: MySQL table */
	dict_table_t* 	innodb_table,	/* in: InnoDB table */
	trx_t*		trx,		/* in: transaction */
	KEY*		key_info,	/* in: Indexes to be created */
	ulint		num_of_keys)	/* in: Number of indexes to
					be created */
{
	Field*		field;
	ulint		key_num;
	int		error = 0;
	ibool		is_unsigned;

	ut_ad(table && innodb_table && trx && key_info && num_of_keys);

	for (key_num = 0; key_num < num_of_keys; key_num++) {
		KEY*		key;

		key = &(key_info[key_num]);

		/* Check that the same index name does not appear
		twice in indexes to be created. */

		for (ulint i = 0; i < key_num; i++) {
			KEY*		key2;

			key2 = &key_info[i];

			if (0 == strcmp(key->name, key2->name)) {
				ut_print_timestamp(stderr);

				fputs("  InnoDB: Error: index ", stderr);
				ut_print_name(stderr, trx, FALSE, key->name);
				fputs(" appears twice in create index\n",
								stderr);

				error = ER_WRONG_NAME_FOR_INDEX;

				return(error);
			}
		}

		/* Check that MySQL does not try to create a column
		prefix index field on an inappropriate data type and
		that the same colum does not appear twice in the index. */

		for (ulint i = 0; i < key->key_parts; i++) {
			KEY_PART_INFO*	key_part1;
			ulint		col_type;	/* Column type */

			key_part1 = key->key_part + i;

			field = key_part1->field;

			col_type = get_innobase_type_from_mysql_type(
					&is_unsigned, field);

			if (DATA_BLOB == col_type
			|| (key_part1->length < field->pack_length()
				&& field->type() != MYSQL_TYPE_VARCHAR)
			|| (field->type() == MYSQL_TYPE_VARCHAR
				&& key_part1->length < field->pack_length()
			          - ((Field_varstring*)field)->length_bytes)) {

				if (col_type == DATA_INT
				    || col_type == DATA_FLOAT
				    || col_type == DATA_DOUBLE
				    || col_type == DATA_DECIMAL) {
					fprintf(stderr,
"InnoDB: error: MySQL is trying to create a column prefix index field\n"
"InnoDB: on an inappropriate data type. Table name %s, column name %s.\n",
						innodb_table->name,
						field->field_name);

					error = ER_WRONG_KEY_COLUMN;
				}
			}

			for (ulint j = 0; j < i; j++) {
				KEY_PART_INFO*	key_part2;

				key_part2 = key->key_part + j;

				if (0 == strcmp(
					key_part1->field->field_name,
					key_part2->field->field_name)) {

					ut_print_timestamp(stderr);

					fputs("  InnoDB: Error: column ",
								stderr);

					ut_print_name(stderr, trx, FALSE,
					key_part1->field->field_name);

					fputs(" appears twice in ", stderr);

					ut_print_name(stderr, trx, FALSE,
								key->name);
					fputs("\n"
"  InnoDB: This is not allowed in InnoDB.\n",
						stderr);

					error = ER_WRONG_KEY_COLUMN;

					return(error);
				}
			}
		}

	}

	return(error);
}

/***********************************************************************
Create index field definition for key part */
static
void
innobase_create_index_field_def(
/*============================*/
	KEY_PART_INFO*		key_part,	/* in: MySQL key definition */
	mem_heap_t*		heap,		/* in: memory heap */
	merge_index_field_t*	index_field)	/* out: index field
						definition for key_part */
{
	Field*		field;
	ibool		is_unsigned;
	ulint		col_type;

	DBUG_ENTER("innobase_create_index_field_def");

	ut_ad(key_part);
	ut_ad(index_field);

	field = key_part->field;
	ut_a(field);

	col_type = get_innobase_type_from_mysql_type(&is_unsigned, field);

	if (DATA_BLOB == col_type
	    || (key_part->length < field->pack_length()
		&& field->type() != MYSQL_TYPE_VARCHAR)
	    || (field->type() == MYSQL_TYPE_VARCHAR
		&& key_part->length < field->pack_length()
			- ((Field_varstring*)field)->length_bytes)) {

		index_field->prefix_len = key_part->length;
	} else {
		index_field->prefix_len = 0;
	}

	index_field->field_name = mem_heap_strdup(heap, field->field_name);

	DBUG_VOID_RETURN;
}

/***********************************************************************
Create index definition for key */
static
void
innobase_create_index_def(
/*======================*/
	KEY*			key,		/* in: key definition */
	bool			new_primary,	/* in: TRUE=generating
						a new primary key
						on the table */
	bool			key_primary,	/* in: TRUE if this key
						is a primary key */
	merge_index_def_t*	index,		/* out: index definition */
	mem_heap_t*		heap)		/* in: heap where memory
						is allocated */
{
	ulint	i;
	ulint	len;
	ulint	n_fields = key->key_parts;
	char*	index_name;

	DBUG_ENTER("innobase_create_index_def");

	index->fields = (merge_index_field_t*) mem_heap_alloc(
		heap, n_fields * sizeof *index->fields);

	index->ind_type = 0;
	index->n_fields = n_fields;
	len = strlen(key->name) + 1;
	index->name = index_name = (char*) mem_heap_alloc(heap,
							  len + !new_primary);

	if (UNIV_LIKELY(!new_primary)) {
		*index_name++ = TEMP_INDEX_PREFIX;
	}

	memcpy(index_name, key->name, len);

	if (key->flags & HA_NOSAME) {
		index->ind_type |= DICT_UNIQUE;
	}

	if (key_primary) {
		index->ind_type |= DICT_CLUSTERED;
	}

	for (i = 0; i < n_fields; i++) {
		innobase_create_index_field_def(&key->key_part[i], heap,
						&index->fields[i]);
	}

	DBUG_VOID_RETURN;
}

/***********************************************************************
Copy index field definition */
static
void
innobase_copy_index_field_def(
/*==========================*/
	const dict_field_t*	field,		/* in: definition to copy */
	merge_index_field_t*	index_field)	/* out: copied definition */
{
	DBUG_ENTER("innobase_copy_index_field_def");
	DBUG_ASSERT(field != NULL);
	DBUG_ASSERT(index_field != NULL);

	index_field->field_name = field->name;
	index_field->prefix_len = field->prefix_len;

	DBUG_VOID_RETURN;
}

/***********************************************************************
Copy index definition for the index */
static
void
innobase_copy_index_def(
/*====================*/
	const dict_index_t*	index,	/* in: index definition to copy */
	merge_index_def_t*	new_index,/* out: Index definition */
	mem_heap_t*		heap)	/* in: heap where allocated */
{
	ulint	n_fields;
	ulint	i;

	DBUG_ENTER("innobase_copy_index_def");

	/* Note that we take only those fields that user defined to be
	in the index.  In the internal representation more colums were
	added and those colums are not copied .*/

	n_fields = index->n_user_defined_cols;

	new_index->fields = (merge_index_field_t*) mem_heap_alloc(
		heap, n_fields * sizeof *new_index->fields);

	/* When adding a PRIMARY KEY, we may convert a previous
	clustered index to a secondary index (UNIQUE NOT NULL). */
	new_index->ind_type = index->type & ~DICT_CLUSTERED;
	new_index->n_fields = n_fields;
	new_index->name = index->name;

	for (i = 0; i < n_fields; i++) {
		innobase_copy_index_field_def(&index->fields[i],
					      &new_index->fields[i]);
	}

	DBUG_VOID_RETURN;
}

/***********************************************************************
Create an index table where indexes are ordered as follows:

IF a new primary key is defined for the table THEN

	1) New primary key
	2) Original secondary indexes
	3) New secondary indexes

ELSE

	1) All new indexes in the order they arrive from MySQL

ENDIF

*/
static
merge_index_def_t*
innobase_create_key_def(
/*====================*/
					/* out: key definitions or NULL */
	trx_t*		trx,		/* in: trx */
	const dict_table_t*table,		/* in: table definition */
	mem_heap_t*	heap,		/* in: heap where space for key
					definitions are allocated */
	KEY*		key_info,	/* in: Indexes to be created */
	ulint&		n_keys)		/* in/out: Number of indexes to
					be created */
{
	ulint			i = 0;
	merge_index_def_t*	indexdef;
	merge_index_def_t*	indexdefs;
	bool			new_primary;

	DBUG_ENTER("innobase_create_key_def");

	indexdef = indexdefs = (merge_index_def_t*)
		mem_heap_alloc(heap, sizeof *indexdef
			       * (n_keys + UT_LIST_GET_LEN(table->indexes)));

	/* If there is a primary key, it is always the first index
	defined for the table. */

	new_primary = !my_strcasecmp(system_charset_info,
				     key_info->name, "PRIMARY");

	/* If there is a UNIQUE INDEX consisting entirely of NOT NULL
	columns, MySQL will treat it as a PRIMARY KEY unless the
	table already has one. */

	if (!new_primary && (key_info->flags & HA_NOSAME)
	    && row_table_got_default_clust_index(table)) {
		uint	key_part = key_info->key_parts;

		new_primary = TRUE;

		while (key_part--) {
			if (key_info->key_part[key_part].key_type
			    & FIELDFLAG_MAYBE_NULL) {
				new_primary = FALSE;
				break;
			}
		}
	}

	if (new_primary) {
		const dict_index_t*	index;

		/* Create the PRIMARY key index definition */
		innobase_create_index_def(&key_info[i++], TRUE, TRUE,
					  indexdef++, heap);

		row_mysql_lock_data_dictionary(trx);

		index = dict_table_get_first_index(table);

		/* Copy the index definitions of the old table.  Skip
		the old clustered index if it is a generated clustered
		index or a PRIMARY KEY.  If the clustered index is a
		UNIQUE INDEX, it must be converted to a secondary index. */

		if (dict_index_get_nth_col(index, 0)->mtype == DATA_SYS
		    || !my_strcasecmp(system_charset_info,
				      index->name, "PRIMARY")) {
			index = dict_table_get_next_index(index);
		}

		while (index) {
			innobase_copy_index_def(index, indexdef++, heap);
			index = dict_table_get_next_index(index);
		}

		row_mysql_unlock_data_dictionary(trx);
	}

	/* Create definitions for added secondary indexes. */

	while (i < n_keys) {
		innobase_create_index_def(&key_info[i++], new_primary, FALSE,
					  indexdef++, heap);
	}

	n_keys = indexdef - indexdefs;

	DBUG_RETURN(indexdefs);
}

/***********************************************************************
Create a temporary tablename using query id, thread id, and id */
static
char*
innobase_create_temporary_tablename(
/*================================*/
					/* out: temporary tablename */
	mem_heap_t*	heap,		/* in: memory heap */
	char		id,		/* in: identifier [0-9a-zA-Z] */
	const char*     table_name)	/* in: table name */
{
	char*			name;
	ulint			len;
	static const char	suffix[] = "@0023 "; /* "# " */

	len = strlen(table_name);

	name = (char*) mem_heap_alloc(heap, len + sizeof suffix);
	memcpy(name, table_name, len);
	memcpy(name + len, suffix, sizeof suffix);
	name[len + (sizeof suffix - 2)] = id;

	return(name);
}

/***********************************************************************
Create indexes. */

int
ha_innobase::add_index(
/*===================*/
				/* out: 0 or error number */
	TABLE*	table,		/* in: Table where indexes are created */
	KEY*	key_info,	/* in: Indexes to be created */
	uint	num_of_keys)	/* in: Number of indexes to be created */
{
	dict_index_t**	index;		/* Index to be created */
	dict_table_t*	innodb_table;	/* InnoDB table in dictionary */
	dict_table_t*	indexed_table;	/* Table where indexes are created */
	merge_index_def_t* index_defs;	/* Index definitions */
	mem_heap_t*     heap;		/* Heap for index definitions */
	trx_t*		trx;		/* Transaction */
	ulint		num_of_idx;
	ulint		num_created	= 0;
	ibool		dict_locked	= FALSE;
	ulint		new_primary;
	ulint		error;

	DBUG_ENTER("ha_innobase::add_index");
	ut_a(table);
	ut_a(key_info);
	ut_a(num_of_keys);

	if (srv_created_new_raw || srv_force_recovery) {
		DBUG_RETURN(HA_ERR_WRONG_COMMAND);
	}

	update_thd();

	heap = mem_heap_create(1024);

	/* In case MySQL calls this in the middle of a SELECT query, release
	possible adaptive hash latch to avoid deadlocks of threads. */
	trx_search_latch_release_if_reserved(prebuilt->trx);

	trx = trx_allocate_for_mysql();
	trx_start_if_not_started(trx);

	trans_register_ha(user_thd, FALSE, ht);

	trx->mysql_thd = user_thd;
	trx->mysql_query_str = thd_query(user_thd);

	innodb_table = indexed_table
		= dict_table_get(prebuilt->table->name, FALSE);

	/* Check that index keys are sensible */

	error = innobase_check_index_keys(
		table, innodb_table, trx, key_info, num_of_keys);

	if (UNIV_UNLIKELY(error)) {
err_exit:
		mem_heap_free(heap);
		trx_general_rollback_for_mysql(trx, FALSE, NULL);
		trx_free_for_mysql(trx);
		DBUG_RETURN(error);
	}

	/* Create table containing all indexes to be built in this
	alter table add index so that they are in the correct order
	in the table. */

	num_of_idx = num_of_keys;

	index_defs = innobase_create_key_def(
		trx, innodb_table, heap, key_info, num_of_idx);

	new_primary = DICT_CLUSTERED & index_defs[0].ind_type;

	/* Allocate memory for dictionary index definitions */

	index = (dict_index_t**) mem_heap_alloc(
		heap, num_of_idx * sizeof *index);

	/* Latch the InnoDB data dictionary exclusively so that no deadlocks
	or lock waits can happen in it during an index create operation. */

	row_mysql_lock_data_dictionary(trx);
	dict_locked = TRUE;

	/* Flag this transaction as a dictionary operation, so that
	the data dictionary will be locked in crash recovery.  Prevent
	warnings if row_merge_lock_table() results in a lock wait. */
	trx_set_dict_operation(trx, TRX_DICT_OP_INDEX_MAY_WAIT);

	/* Acquire an exclusive lock on the table
	before creating any indexes. */
	error = row_merge_lock_table(trx, innodb_table);

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {

		goto error_handling;
	}

	trx_set_dict_operation(trx, TRX_DICT_OP_INDEX);

	/* If a new primary key is defined for the table we need
	to drop the original table and rebuild all indexes. */

	if (UNIV_UNLIKELY(new_primary)) {
		char*	new_table_name = innobase_create_temporary_tablename(
			heap, '1', innodb_table->name);

		/* Clone the table. */
		trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);
		indexed_table = row_merge_create_temporary_table(
			new_table_name, index_defs, innodb_table, trx);

		if (!indexed_table) {

			switch (trx->error_state) {
			case DB_TABLESPACE_ALREADY_EXISTS:
			case DB_DUPLICATE_KEY:
				innobase_convert_tablename(new_table_name);
				my_error(HA_ERR_TABLE_EXIST, MYF(0),
					 new_table_name);
				error = HA_ERR_TABLE_EXIST;
				break;
			default:
				error = convert_error_code_to_mysql(
					trx->error_state, user_thd);
			}

			row_mysql_unlock_data_dictionary(trx);
			goto err_exit;
		}

		trx->table_id = indexed_table->id;
	}

	/* Create the indexes in SYS_INDEXES and load into dictionary. */

	for (ulint i = 0; i < num_of_idx; i++) {

		index[i] = row_merge_create_index(trx, indexed_table,
						  &index_defs[i]);

		if (!index[i]) {
			error = trx->error_state;
			goto error_handling;
		}

		num_created++;
	}

	ut_ad(error == DB_SUCCESS);

	/* Raise version number of the table to track this table's
	definition changes. */

	indexed_table->version_number++;

	row_mysql_unlock_data_dictionary(trx);
	dict_locked = FALSE;

	ut_a(trx->n_active_thrs == 0);
	ut_a(UT_LIST_GET_LEN(trx->signals) == 0);

	if (UNIV_UNLIKELY(new_primary)) {
		/* A primary key is to be built.  Acquire an exclusive
		table lock also on the table that is being created. */
		ut_ad(indexed_table != innodb_table);

		error = row_merge_lock_table(trx, indexed_table);

		if (UNIV_UNLIKELY(error != DB_SUCCESS)) {

			goto error_handling;
		}
	}

	/* Read the clustered index of the table and build indexes
	based on this information using temporary files and merge sort. */
	error = row_merge_build_indexes(trx, innodb_table, indexed_table,
					index, num_of_idx, table);

error_handling:
#ifdef UNIV_DEBUG
	/* TODO: At the moment we can't handle the following statement
	in our debugging code below:

	alter table t drop index b, add index (b);

	The fix will have to parse the SQL and note that the index
	being added has the same name as the the one being dropped and
	ignore that in the dup index check.*/
	//dict_table_check_for_dup_indexes(prebuilt->table);
#endif

	/* After an error, remove all those index definitions from the
	dictionary which were defined. */

	switch (error) {
		const char*	old_name;
		char*		tmp_name;
	case DB_SUCCESS:
		ut_ad(!dict_locked);

		if (!new_primary) {
			error = row_merge_rename_indexes(trx, indexed_table);

			if (error != DB_SUCCESS) {
				row_merge_drop_indexes(trx, indexed_table,
						       index, num_created);
			}

			goto convert_error;
		}

		/* If a new primary key was defined for the table and
		there was no error at this point, we can now rename
		the old table as a temporary table, rename the new
		temporary table as the old table and drop the old table. */
		old_name = innodb_table->name;
		tmp_name = innobase_create_temporary_tablename(heap, '2',
							       old_name);

		row_mysql_lock_data_dictionary(trx);
		dict_locked = TRUE;

		error = row_merge_rename_tables(innodb_table, indexed_table,
						tmp_name, trx);

		if (error != DB_SUCCESS) {

			row_merge_drop_table(trx, indexed_table);

			switch (error) {
			case DB_TABLESPACE_ALREADY_EXISTS:
			case DB_DUPLICATE_KEY:
				innobase_convert_tablename(tmp_name);
				my_error(HA_ERR_TABLE_EXIST, MYF(0), tmp_name);
				error = HA_ERR_TABLE_EXIST;
				break;
			default:
				error = convert_error_code_to_mysql(
					trx->error_state, user_thd);
			}
			break;
		}

		row_prebuilt_table_obsolete(innodb_table);

		row_prebuilt_free(prebuilt, TRUE);
		prebuilt = row_create_prebuilt(indexed_table);

		prebuilt->table->n_mysql_handles_opened++;

		/* Drop the old table if there are no open views
		referring to it.  If there are such views, we will
		drop the table when we free the prebuilts and there
		are no more references to it. */

		error = row_merge_drop_table(trx, innodb_table);
		goto convert_error;

	case DB_TOO_BIG_RECORD:
		my_error(HA_ERR_TO_BIG_ROW, MYF(0));
		goto error;
	case DB_PRIMARY_KEY_IS_NULL:
		my_error(ER_PRIMARY_CANT_HAVE_NULL, MYF(0));
		/* fall through */
	case DB_DUPLICATE_KEY:
error:
		prebuilt->trx->error_info = NULL;
		prebuilt->trx->error_key_num = trx->error_key_num;
		/* fall through */
	default:
		if (new_primary) {
			row_merge_drop_table(trx, indexed_table);
		} else {
			row_merge_drop_indexes(trx, indexed_table,
					       index, num_created);
		}

convert_error:
		error = convert_error_code_to_mysql(error, user_thd);
	}

	mem_heap_free(heap);
	trx_commit_for_mysql(trx);

	if (dict_locked) {
		row_mysql_unlock_data_dictionary(trx);
	}

	trx_free_for_mysql(trx);

	/* There might be work for utility threads.*/
	srv_active_wake_master_thread();

	DBUG_RETURN(error);
}

/***********************************************************************
Prepare to drop some indexes of a table. */

int
ha_innobase::prepare_drop_index(
/*============================*/
				/* out: 0 or error number */
	TABLE*	table,		/* in: Table where indexes are dropped */
	uint*	key_num,	/* in: Key nums to be dropped */
	uint	num_of_keys)	/* in: Number of keys to be dropped */
{
	trx_t*		trx;
	int		err = 0;
	uint 		n_key;

	DBUG_ENTER("ha_innobase::prepare_drop_index");
	ut_ad(table);
	ut_ad(key_num);
	ut_ad(num_of_keys);
	if (srv_created_new_raw || srv_force_recovery) {
		DBUG_RETURN(HA_ERR_WRONG_COMMAND);
	}

	update_thd();

	trx_search_latch_release_if_reserved(prebuilt->trx);
	trx = prebuilt->trx;

	/* Test and mark all the indexes to be dropped */

	row_mysql_lock_data_dictionary(trx);

	for (n_key = 0; n_key < num_of_keys; n_key++) {
		const KEY*	key;
		dict_index_t*	index;

		key = table->key_info + key_num[n_key];
		index = dict_table_get_index_on_name_and_min_id(
			prebuilt->table, key->name);

		if (!index) {
			sql_print_error("InnoDB could not find key n:o %u "
					"with name %s for table %s",
					key_num[n_key],
					key ? key->name : "NULL",
					prebuilt->table->name);

			err = HA_ERR_KEY_NOT_FOUND;
			goto func_exit;
		}

		/* Refuse to drop the clustered index.  It would be
		better to automatically generate a clustered index,
		but mysql_alter_table() will call this method only
		after ha_innobase::add_index(). */

		if (dict_index_is_clust(index)) {
			my_error(ER_REQUIRES_PRIMARY_KEY, MYF(0));
			err = -1;
			goto func_exit;
		}

		index->to_be_dropped = TRUE;
	}

	/* If FOREIGN_KEY_CHECK = 1 you may not drop an index defined
	for a foreign key constraint because InnoDB requires that both
	tables contain indexes for the constraint.  Note that CREATE
	INDEX id ON table does a CREATE INDEX and DROP INDEX, and we
	can ignore here foreign keys because a new index for the
	foreign key has already been created.

	We check for the foreign key constraints after marking the
	candidate indexes for deletion, because when we check for an
	equivalent foreign index we don't want to select an index that
	is later deleted. */

	if (trx->check_foreigns
	    && thd_sql_command(user_thd) != SQLCOM_CREATE_INDEX) {
		for (n_key = 0; n_key < num_of_keys; n_key++) {
			KEY*		key;
			dict_index_t*	index;
			dict_foreign_t* foreign;

			key = table->key_info + key_num[n_key];
			index = dict_table_get_index_on_name_and_min_id(
				prebuilt->table, key->name);

			ut_a(index);
			ut_a(index->to_be_dropped);

			/* Check if the index is referenced. */
			foreign = dict_table_get_referenced_constraint(
				prebuilt->table, index);

			if (foreign) {
index_needed:
				trx_set_detailed_error(
					trx,
					"Index needed in foreign key "
					"constraint");

				trx->error_info = index;

				err = HA_ERR_DROP_INDEX_FK;
				break;
			} else {
				/* Check if this index references some
				other table */
				foreign = dict_table_get_foreign_constraint(
					prebuilt->table, index);

				if (foreign) {
					ut_a(foreign->foreign_index == index);

					/* Search for an equivalent index that
					the foreign key contraint could use
					if this index were to be deleted. */
					if (!dict_table_find_equivalent_index(
						prebuilt->table,
						foreign->foreign_index)) {

						goto index_needed;
					}
				}
			}
		}
	}

func_exit:
	if (err) {
		/* Undo our changes since there was some sort of error */
		for (n_key = 0; n_key < num_of_keys; n_key++) {
			const KEY*	key;
			dict_index_t*	index;

			key = table->key_info + key_num[n_key];
			index = dict_table_get_index_on_name_and_min_id(
				prebuilt->table, key->name);

			if (index) {
				index->to_be_dropped = FALSE;
			}
		}
	}

	row_mysql_unlock_data_dictionary(trx);

	DBUG_RETURN(err);
}

/***********************************************************************
Drop the indexes that were passed to a successful prepare_drop_index(). */

int
ha_innobase::final_drop_index(
/*==========================*/
				/* out: 0 or error number */
	TABLE*	table)		/* in: Table where indexes are dropped */
{
	dict_index_t*	index;		/* Index to be dropped */
	trx_t*		trx;		/* Transaction */

	DBUG_ENTER("ha_innobase::final_drop_index");
	ut_ad(table);

	if (srv_created_new_raw || srv_force_recovery) {
		DBUG_RETURN(HA_ERR_WRONG_COMMAND);
	}

	update_thd();

	trx_search_latch_release_if_reserved(prebuilt->trx);
	trx = prebuilt->trx;

	/* Drop indexes marked to be dropped */

	row_mysql_lock_data_dictionary(trx);

	index = dict_table_get_first_index(prebuilt->table);

	while (index) {
		dict_index_t*	next_index;

		next_index = dict_table_get_next_index(index);

		if (index->to_be_dropped) {

			row_merge_drop_index(index, prebuilt->table, trx);
		}

		index = next_index;
	}

	prebuilt->table->version_number++;

#ifdef UNIV_DEBUG
	dict_table_check_for_dup_indexes(prebuilt->table);
#endif

	row_mysql_unlock_data_dictionary(trx);

	/* Flush the log to reduce probability that the .frm files and
	the InnoDB data dictionary get out-of-sync if the user runs
	with innodb_flush_log_at_trx_commit = 0 */

	log_buffer_flush_to_disk();

	/* Tell the InnoDB server that there might be work for
	utility threads: */

	srv_active_wake_master_thread();

	trx_commit_for_mysql(trx);

	DBUG_RETURN(0);
}