/******************************************************
Insert into a table

(c) 1996 Innobase Oy

Created 4/20/1996 Heikki Tuuri
*******************************************************/

#ifndef row0ins_h
#define row0ins_h

#include "univ.i"
#include "data0data.h"
#include "que0types.h"
#include "dict0types.h"
#include "trx0types.h"
#include "row0types.h"

/*******************************************************************
Checks if foreign key constraint fails for an index entry. Sets shared locks
which lock either the success or the failure of the constraint. NOTE that
the caller must have a shared latch on dict_foreign_key_check_lock. */
UNIV_INTERN
ulint
row_ins_check_foreign_constraint(
/*=============================*/
				/* out: DB_SUCCESS, DB_LOCK_WAIT,
				DB_NO_REFERENCED_ROW,
				or DB_ROW_IS_REFERENCED */
	ibool		check_ref,/* in: TRUE If we want to check that
				the referenced table is ok, FALSE if we
				want to to check the foreign key table */
	dict_foreign_t*	foreign,/* in: foreign constraint; NOTE that the
				tables mentioned in it must be in the
				dictionary cache if they exist at all */
	dict_table_t*	table,	/* in: if check_ref is TRUE, then the foreign
				table, else the referenced table */
	dtuple_t*	entry,	/* in: index entry for index */
	que_thr_t*	thr);	/* in: query thread */
/*************************************************************************
Creates an insert node struct. */
UNIV_INTERN
ins_node_t*
ins_node_create(
/*============*/
					/* out, own: insert node struct */
	ulint		ins_type,	/* in: INS_VALUES, ... */
	dict_table_t*	table,		/* in: table where to insert */
	mem_heap_t*	heap);		/* in: mem heap where created */
/*************************************************************************
Sets a new row to insert for an INS_DIRECT node. This function is only used
if we have constructed the row separately, which is a rare case; this
function is quite slow. */
UNIV_INTERN
void
ins_node_set_new_row(
/*=================*/
	ins_node_t*	node,	/* in: insert node */
	dtuple_t*	row);	/* in: new row (or first row) for the node */
/*******************************************************************
Inserts an index entry to index. Tries first optimistic, then pessimistic
descent down the tree. If the entry matches enough to a delete marked record,
performs the insert by updating or delete unmarking the delete marked
record. */
UNIV_INTERN
ulint
row_ins_index_entry(
/*================*/
				/* out: DB_SUCCESS, DB_LOCK_WAIT,
				DB_DUPLICATE_KEY, or some other error code */
	dict_index_t*	index,	/* in: index */
	dtuple_t*	entry,	/* in: index entry to insert */
	ulint		n_ext,	/* in: number of externally stored columns */
	ibool		foreign,/* in: TRUE=check foreign key constraints */
	que_thr_t*	thr);	/* in: query thread */
/***************************************************************
Inserts a row to a table. This is a high-level function used in
SQL execution graphs. */
UNIV_INTERN
que_thr_t*
row_ins_step(
/*=========*/
				/* out: query thread to run next or NULL */
	que_thr_t*	thr);	/* in: query thread */
/***************************************************************
Creates an entry template for each index of a table. */
UNIV_INTERN
void
ins_node_create_entry_list(
/*=======================*/
	ins_node_t*	node);	/* in: row insert node */

/* Insert node structure */

struct ins_node_struct{
	que_common_t	common;	/* node type: QUE_NODE_INSERT */
	ulint		ins_type;/* INS_VALUES, INS_SEARCHED, or INS_DIRECT */
	dtuple_t*	row;	/* row to insert */
	dict_table_t*	table;	/* table where to insert */
	sel_node_t*	select;	/* select in searched insert */
	que_node_t*	values_list;/* list of expressions to evaluate and
				insert in an INS_VALUES insert */
	ulint		state;	/* node execution state */
	dict_index_t*	index;	/* NULL, or the next index where the index
				entry should be inserted */
	dtuple_t*	entry;	/* NULL, or entry to insert in the index;
				after a successful insert of the entry,
				this should be reset to NULL */
	UT_LIST_BASE_NODE_T(dtuple_t)
			entry_list;/* list of entries, one for each index */
	byte*		row_id_buf;/* buffer for the row id sys field in row */
	dulint		trx_id;	/* trx id or the last trx which executed the
				node */
	byte*		trx_id_buf;/* buffer for the trx id sys field in row */
	mem_heap_t*	entry_sys_heap;
				/* memory heap used as auxiliary storage;
				entry_list and sys fields are stored here;
				if this is NULL, entry list should be created
				and buffers for sys fields in row allocated */
	ulint		magic_n;
};

#define	INS_NODE_MAGIC_N	15849075

/* Insert node types */
#define INS_SEARCHED	0	/* INSERT INTO ... SELECT ... */
#define INS_VALUES	1	/* INSERT INTO ... VALUES ... */
#define INS_DIRECT	2	/* this is for internal use in dict0crea:
				insert the row directly */

/* Node execution states */
#define	INS_NODE_SET_IX_LOCK	1	/* we should set an IX lock on table */
#define INS_NODE_ALLOC_ROW_ID	2	/* row id should be allocated */
#define	INS_NODE_INSERT_ENTRIES 3	/* index entries should be built and
					inserted */

#ifndef UNIV_NONINL
#include "row0ins.ic"
#endif

#endif
