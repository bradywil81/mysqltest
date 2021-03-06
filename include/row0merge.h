/******************************************************
Index build routines using a merge sort

(c) 2005 Innobase Oy

Created 13/06/2005 Jan Lindstrom
*******************************************************/

#ifndef row0merge_h
#define row0merge_h

#include "univ.i"
#include "data0data.h"
#include "dict0types.h"
#include "trx0types.h"
#include "que0types.h"
#include "mtr0mtr.h"
#include "rem0types.h"
#include "rem0rec.h"
#include "read0types.h"
#include "btr0types.h"
#include "row0mysql.h"
#include "lock0types.h"

/* This structure holds index field definitions */

struct merge_index_field_struct {
	ulint		prefix_len;	/* Prefix len */
	const char*	field_name;	/* Field name */
};

typedef struct merge_index_field_struct merge_index_field_t;

/* This structure holds index definitions */

struct merge_index_def_struct {
	const char*		name;		/* Index name */
	ulint			ind_type;	/* 0, DICT_UNIQUE,
						or DICT_CLUSTERED */
	ulint			n_fields;	/* Number of fields in index */
	merge_index_field_t*	fields;		/* Field definitions */
};

typedef struct merge_index_def_struct merge_index_def_t;

/*************************************************************************
Sets an exclusive lock on a table, for the duration of creating indexes. */
UNIV_INTERN
ulint
row_merge_lock_table(
/*=================*/
					/* out: error code or DB_SUCCESS */
	trx_t*		trx,		/* in/out: transaction */
	dict_table_t*	table,		/* in: table to lock */
	enum lock_mode	mode);		/* in: LOCK_X or LOCK_S */
/*************************************************************************
Drop an index from the InnoDB system tables. */
UNIV_INTERN
void
row_merge_drop_index(
/*=================*/
	dict_index_t*	index,	/* in: index to be removed */
	dict_table_t*	table,	/* in: table */
	trx_t*		trx);	/* in: transaction handle */
/*************************************************************************
Drop those indexes which were created before an error occurred
when building an index. */
UNIV_INTERN
void
row_merge_drop_indexes(
/*===================*/
	trx_t*		trx,		/* in: transaction */
	dict_table_t*	table,		/* in: table containing the indexes */
	dict_index_t**	index,		/* in: indexes to drop */
	ulint		num_created);	/* in: number of elements in index[] */
/*************************************************************************
Drop all partially created indexes during crash recovery. */
UNIV_INTERN
void
row_merge_drop_temp_indexes(void);
/*=============================*/
/*************************************************************************
Rename the tables in the data dictionary. */
UNIV_INTERN
ulint
row_merge_rename_tables(
/*====================*/
					/* out: error code or DB_SUCCESS */
	dict_table_t*	old_table,	/* in/out: old table, renamed to
					tmp_name */
	dict_table_t*	new_table,	/* in/out: new table, renamed to
					old_table->name */
	const char*	tmp_name,	/* in: new name for old_table */
	trx_t*		trx);		/* in: transaction handle */

/*************************************************************************
Create a temporary table for creating a primary key, using the definition
of an existing table. */
UNIV_INTERN
dict_table_t*
row_merge_create_temporary_table(
/*=============================*/
						/* out: table,
						or NULL on error */
	const char*		table_name,	/* in: new table name */
	const merge_index_def_t*index_def,	/* in: the index definition
						of the primary key */
	const dict_table_t*	table,		/* in: old table definition */
	trx_t*			trx);		/* in/out: transaction
						(sets error_state) */
/*************************************************************************
Rename the temporary indexes in the dictionary to permanent ones. */
UNIV_INTERN
ulint
row_merge_rename_indexes(
/*=====================*/
					/* out: DB_SUCCESS if all OK */
	trx_t*		trx,		/* in/out: transaction */
	dict_table_t*	table);		/* in/out: table with new indexes */
/*************************************************************************
Create the index and load in to the dictionary. */
UNIV_INTERN
dict_index_t*
row_merge_create_index(
/*===================*/
					/* out: index, or NULL on error */
	trx_t*		trx,		/* in/out: trx (sets error_state) */
	dict_table_t*	table,		/* in: the index is on this table */
	const merge_index_def_t*	/* in: the index definition */
			index_def);
#ifdef ROW_MERGE_IS_INDEX_USABLE
/*************************************************************************
Check if a transaction can use an index. */
UNIV_INTERN
ibool
row_merge_is_index_usable(
/*======================*/
					/* out: TRUE if index can be used by
					the transaction else FALSE*/
	const trx_t*		trx,	/* in: transaction */
	const dict_index_t*	index);	/* in: index to check */
#endif /* ROW_MERGE_IS_INDEX_USABLE */
/*************************************************************************
If there are views that refer to the old table name then we "attach" to
the new instance of the table else we drop it immediately. */
UNIV_INTERN
ulint
row_merge_drop_table(
/*=================*/
					/* out: DB_SUCCESS or error code */
	trx_t*		trx,		/* in: transaction */
	dict_table_t*	table);		/* in: table instance to drop */

/*************************************************************************
Build indexes on a table by reading a clustered index,
creating a temporary file containing index entries, merge sorting
these index entries and inserting sorted index entries to indexes. */
UNIV_INTERN
ulint
row_merge_build_indexes(
/*====================*/
					/* out: DB_SUCCESS or error code */
	trx_t*		trx,		/* in: transaction */
	dict_table_t*	old_table,	/* in: table where rows are
					read from */
	dict_table_t*	new_table,	/* in: table where indexes are
					created; identical to old_table
					unless creating a PRIMARY KEY */
	dict_index_t**	indexes,	/* in: indexes to be created */
	ulint		n_indexes,	/* in: size of indexes[] */
	TABLE*		table);		/* in/out: MySQL table, for
					reporting erroneous key value
					if applicable */
#endif /* row0merge.h */
