/******************************************************
The transaction lock system

(c) 1996 Innobase Oy

Created 5/7/1996 Heikki Tuuri
*******************************************************/

#ifndef lock0lock_h
#define lock0lock_h

#include "univ.i"
#include "buf0types.h"
#include "trx0types.h"
#include "rem0types.h"
#include "dict0types.h"
#include "que0types.h"
#include "lock0types.h"
#include "read0types.h"
#include "hash0hash.h"

#ifdef UNIV_DEBUG
extern ibool	lock_print_waits;
#endif /* UNIV_DEBUG */
/* Buffer for storing information about the most recent deadlock error */
extern FILE*	lock_latest_err_file;

/*************************************************************************
Gets the size of a lock struct. */
UNIV_INTERN
ulint
lock_get_size(void);
/*===============*/
			/* out: size in bytes */
/*************************************************************************
Creates the lock system at database start. */
UNIV_INTERN
void
lock_sys_create(
/*============*/
	ulint	n_cells);	/* in: number of slots in lock hash table */
/*************************************************************************
Checks if some transaction has an implicit x-lock on a record in a clustered
index. */
UNIV_INLINE
trx_t*
lock_clust_rec_some_has_impl(
/*=========================*/
				/* out: transaction which has the x-lock, or
				NULL */
	const rec_t*	rec,	/* in: user record */
	dict_index_t*	index,	/* in: clustered index */
	const ulint*	offsets);/* in: rec_get_offsets(rec, index) */
/*************************************************************************
Gets the heap_no of the smallest user record on a page. */
UNIV_INLINE
ulint
lock_get_min_heap_no(
/*=================*/
					/* out: heap_no of smallest
					user record, or
					PAGE_HEAP_NO_SUPREMUM */
	const buf_block_t*	block);	/* in: buffer block */
/*****************************************************************
Updates the lock table when we have reorganized a page. NOTE: we copy
also the locks set on the infimum of the page; the infimum may carry
locks if an update of a record is occurring on the page, and its locks
were temporarily stored on the infimum. */
UNIV_INTERN
void
lock_move_reorganize_page(
/*======================*/
	const buf_block_t*	block,	/* in: old index page, now
					reorganized */
	const buf_block_t*	oblock);/* in: copy of the old, not
					reorganized page */
/*****************************************************************
Moves the explicit locks on user records to another page if a record
list end is moved to another page. */
UNIV_INTERN
void
lock_move_rec_list_end(
/*===================*/
	const buf_block_t*	new_block,	/* in: index page to move to */
	const buf_block_t*	block,		/* in: index page */
	const rec_t*		rec);		/* in: record on page: this
						is the first record moved */
/*****************************************************************
Moves the explicit locks on user records to another page if a record
list start is moved to another page. */
UNIV_INTERN
void
lock_move_rec_list_start(
/*=====================*/
	const buf_block_t*	new_block,	/* in: index page to move to */
	const buf_block_t*	block,		/* in: index page */
	const rec_t*		rec,		/* in: record on page:
						this is the first
						record NOT copied */
	const rec_t*		old_end);	/* in: old
						previous-to-last
						record on new_page
						before the records
						were copied */
/*****************************************************************
Updates the lock table when a page is split to the right. */
UNIV_INTERN
void
lock_update_split_right(
/*====================*/
	const buf_block_t*	right_block,	/* in: right page */
	const buf_block_t*	left_block);	/* in: left page */
/*****************************************************************
Updates the lock table when a page is merged to the right. */
UNIV_INTERN
void
lock_update_merge_right(
/*====================*/
	const buf_block_t*	right_block,	/* in: right page to
						which merged */
	const rec_t*		orig_succ,	/* in: original
						successor of infimum
						on the right page
						before merge */
	const buf_block_t*	left_block);	/* in: merged index
						page which will be
						discarded */
/*****************************************************************
Updates the lock table when the root page is copied to another in
btr_root_raise_and_insert. Note that we leave lock structs on the
root page, even though they do not make sense on other than leaf
pages: the reason is that in a pessimistic update the infimum record
of the root page will act as a dummy carrier of the locks of the record
to be updated. */
UNIV_INTERN
void
lock_update_root_raise(
/*===================*/
	const buf_block_t*	block,	/* in: index page to which copied */
	const buf_block_t*	root);	/* in: root page */
/*****************************************************************
Updates the lock table when a page is copied to another and the original page
is removed from the chain of leaf pages, except if page is the root! */
UNIV_INTERN
void
lock_update_copy_and_discard(
/*=========================*/
	const buf_block_t*	new_block,	/* in: index page to
						which copied */
	const buf_block_t*	block);		/* in: index page;
						NOT the root! */
/*****************************************************************
Updates the lock table when a page is split to the left. */
UNIV_INTERN
void
lock_update_split_left(
/*===================*/
	const buf_block_t*	right_block,	/* in: right page */
	const buf_block_t*	left_block);	/* in: left page */
/*****************************************************************
Updates the lock table when a page is merged to the left. */
UNIV_INTERN
void
lock_update_merge_left(
/*===================*/
	const buf_block_t*	left_block,	/* in: left page to
						which merged */
	const rec_t*		orig_pred,	/* in: original predecessor
						of supremum on the left page
						before merge */
	const buf_block_t*	right_block);	/* in: merged index page
						which will be discarded */
/*****************************************************************
Resets the original locks on heir and replaces them with gap type locks
inherited from rec. */
UNIV_INTERN
void
lock_rec_reset_and_inherit_gap_locks(
/*=================================*/
	const buf_block_t*	heir_block,	/* in: block containing the
						record which inherits */
	const buf_block_t*	block,		/* in: block containing the
						record from which inherited;
						does NOT reset the locks on
						this record */
	ulint			heir_heap_no,	/* in: heap_no of the
						inheriting record */
	ulint			heap_no);	/* in: heap_no of the
						donating record */
/*****************************************************************
Updates the lock table when a page is discarded. */
UNIV_INTERN
void
lock_update_discard(
/*================*/
	const buf_block_t*	heir_block,	/* in: index page
						which will inherit the locks */
	ulint			heir_heap_no,	/* in: heap_no of the record
						which will inherit the locks */
	const buf_block_t*	block);		/* in: index page
						which will be discarded */
/*****************************************************************
Updates the lock table when a new user record is inserted. */
UNIV_INTERN
void
lock_update_insert(
/*===============*/
	const buf_block_t*	block,	/* in: buffer block containing rec */
	const rec_t*		rec);	/* in: the inserted record */
/*****************************************************************
Updates the lock table when a record is removed. */
UNIV_INTERN
void
lock_update_delete(
/*===============*/
	const buf_block_t*	block,	/* in: buffer block containing rec */
	const rec_t*		rec);	/* in: the record to be removed */
/*************************************************************************
Stores on the page infimum record the explicit locks of another record.
This function is used to store the lock state of a record when it is
updated and the size of the record changes in the update. The record
is in such an update moved, perhaps to another page. The infimum record
acts as a dummy carrier record, taking care of lock releases while the
actual record is being moved. */
UNIV_INTERN
void
lock_rec_store_on_page_infimum(
/*===========================*/
	const buf_block_t*	block,	/* in: buffer block containing rec */
	const rec_t*		rec);	/* in: record whose lock state
					is stored on the infimum
					record of the same page; lock
					bits are reset on the
					record */
/*************************************************************************
Restores the state of explicit lock requests on a single record, where the
state was stored on the infimum of the page. */
UNIV_INTERN
void
lock_rec_restore_from_page_infimum(
/*===============================*/
	const buf_block_t*	block,	/* in: buffer block containing rec */
	const rec_t*		rec,	/* in: record whose lock state
					is restored */
	const buf_block_t*	donator);/* in: page (rec is not
					necessarily on this page)
					whose infimum stored the lock
					state; lock bits are reset on
					the infimum */
/*************************************************************************
Returns TRUE if there are explicit record locks on a page. */
UNIV_INTERN
ibool
lock_rec_expl_exist_on_page(
/*========================*/
			/* out: TRUE if there are explicit record locks on
			the page */
	ulint	space,	/* in: space id */
	ulint	page_no);/* in: page number */
/*************************************************************************
Checks if locks of other transactions prevent an immediate insert of
a record. If they do, first tests if the query thread should anyway
be suspended for some reason; if not, then puts the transaction and
the query thread to the lock wait state and inserts a waiting request
for a gap x-lock to the lock queue. */
UNIV_INTERN
ulint
lock_rec_insert_check_and_lock(
/*===========================*/
				/* out: DB_SUCCESS, DB_LOCK_WAIT,
				DB_DEADLOCK, or DB_QUE_THR_SUSPENDED */
	ulint		flags,	/* in: if BTR_NO_LOCKING_FLAG bit is
				set, does nothing */
	rec_t*		rec,	/* in: record after which to insert */
	buf_block_t*	block,	/* in/out: buffer block of rec */
	dict_index_t*	index,	/* in: index */
	que_thr_t*	thr,	/* in: query thread */
	ibool*		inherit);/* out: set to TRUE if the new
				inserted record maybe should inherit
				LOCK_GAP type locks from the successor
				record */
/*************************************************************************
Checks if locks of other transactions prevent an immediate modify (update,
delete mark, or delete unmark) of a clustered index record. If they do,
first tests if the query thread should anyway be suspended for some
reason; if not, then puts the transaction and the query thread to the
lock wait state and inserts a waiting request for a record x-lock to the
lock queue. */
UNIV_INTERN
ulint
lock_clust_rec_modify_check_and_lock(
/*=================================*/
					/* out: DB_SUCCESS,
					DB_LOCK_WAIT, DB_DEADLOCK, or
					DB_QUE_THR_SUSPENDED */
	ulint			flags,	/* in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/* in: buffer block of rec */
	const rec_t*		rec,	/* in: record which should be
					modified */
	dict_index_t*		index,	/* in: clustered index */
	const ulint*		offsets,/* in: rec_get_offsets(rec, index) */
	que_thr_t*		thr);	/* in: query thread */
/*************************************************************************
Checks if locks of other transactions prevent an immediate modify
(delete mark or delete unmark) of a secondary index record. */
UNIV_INTERN
ulint
lock_sec_rec_modify_check_and_lock(
/*===============================*/
				/* out: DB_SUCCESS, DB_LOCK_WAIT,
				DB_DEADLOCK, or DB_QUE_THR_SUSPENDED */
	ulint		flags,	/* in: if BTR_NO_LOCKING_FLAG
				bit is set, does nothing */
	buf_block_t*	block,	/* in/out: buffer block of rec */
	rec_t*		rec,	/* in: record which should be
				modified; NOTE: as this is a secondary
				index, we always have to modify the
				clustered index record first: see the
				comment below */
	dict_index_t*	index,	/* in: secondary index */
	que_thr_t*	thr);	/* in: query thread */
/*************************************************************************
Like the counterpart for a clustered index below, but now we read a
secondary index record. */
UNIV_INTERN
ulint
lock_sec_rec_read_check_and_lock(
/*=============================*/
					/* out: DB_SUCCESS,
					DB_LOCK_WAIT, DB_DEADLOCK, or
					DB_QUE_THR_SUSPENDED */
	ulint			flags,	/* in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/* in: buffer block of rec */
	const rec_t*		rec,	/* in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/* in: secondary index */
	const ulint*		offsets,/* in: rec_get_offsets(rec, index) */
	enum lock_mode		mode,	/* in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	ulint			gap_mode,/* in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr);	/* in: query thread */
/*************************************************************************
Checks if locks of other transactions prevent an immediate read, or passing
over by a read cursor, of a clustered index record. If they do, first tests
if the query thread should anyway be suspended for some reason; if not, then
puts the transaction and the query thread to the lock wait state and inserts a
waiting request for a record lock to the lock queue. Sets the requested mode
lock on the record. */
UNIV_INTERN
ulint
lock_clust_rec_read_check_and_lock(
/*===============================*/
					/* out: DB_SUCCESS,
					DB_LOCK_WAIT, DB_DEADLOCK, or
					DB_QUE_THR_SUSPENDED */
	ulint			flags,	/* in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/* in: buffer block of rec */
	const rec_t*		rec,	/* in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/* in: clustered index */
	const ulint*		offsets,/* in: rec_get_offsets(rec, index) */
	enum lock_mode		mode,	/* in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	ulint			gap_mode,/* in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr);	/* in: query thread */
/*************************************************************************
Checks if locks of other transactions prevent an immediate read, or passing
over by a read cursor, of a clustered index record. If they do, first tests
if the query thread should anyway be suspended for some reason; if not, then
puts the transaction and the query thread to the lock wait state and inserts a
waiting request for a record lock to the lock queue. Sets the requested mode
lock on the record. This is an alternative version of
lock_clust_rec_read_check_and_lock() that does not require the parameter
"offsets". */
UNIV_INTERN
ulint
lock_clust_rec_read_check_and_lock_alt(
/*===================================*/
					/* out: DB_SUCCESS,
					DB_LOCK_WAIT, DB_DEADLOCK, or
					DB_QUE_THR_SUSPENDED */
	ulint			flags,	/* in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/* in: buffer block of rec */
	const rec_t*		rec,	/* in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/* in: clustered index */
	enum lock_mode		mode,	/* in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	ulint			gap_mode,/* in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr);	/* in: query thread */
/*************************************************************************
Checks that a record is seen in a consistent read. */
UNIV_INTERN
ibool
lock_clust_rec_cons_read_sees(
/*==========================*/
				/* out: TRUE if sees, or FALSE if an earlier
				version of the record should be retrieved */
	const rec_t*	rec,	/* in: user record which should be read or
				passed over by a read cursor */
	dict_index_t*	index,	/* in: clustered index */
	const ulint*	offsets,/* in: rec_get_offsets(rec, index) */
	read_view_t*	view);	/* in: consistent read view */
/*************************************************************************
Checks that a non-clustered index record is seen in a consistent read. */
UNIV_INTERN
ulint
lock_sec_rec_cons_read_sees(
/*========================*/
					/* out: TRUE if certainly
					sees, or FALSE if an earlier
					version of the clustered index
					record might be needed: NOTE
					that a non-clustered index
					page contains so little
					information on its
					modifications that also in the
					case FALSE, the present
					version of rec may be the
					right, but we must check this
					from the clustered index
					record */
	const rec_t*		rec,	/* in: user record which
					should be read or passed over
					by a read cursor */
	const read_view_t*	view);	/* in: consistent read view */
/*************************************************************************
Locks the specified database table in the mode given. If the lock cannot
be granted immediately, the query thread is put to wait. */
UNIV_INTERN
ulint
lock_table(
/*=======*/
				/* out: DB_SUCCESS, DB_LOCK_WAIT,
				DB_DEADLOCK, or DB_QUE_THR_SUSPENDED */
	ulint		flags,	/* in: if BTR_NO_LOCKING_FLAG bit is set,
				does nothing */
	dict_table_t*	table,	/* in: database table in dictionary cache */
	enum lock_mode	mode,	/* in: lock mode */
	que_thr_t*	thr);	/* in: query thread */
/*************************************************************************
Checks if there are any locks set on the table. */
UNIV_INTERN
ibool
lock_is_on_table(
/*=============*/
				/* out: TRUE if there are lock(s) */
	dict_table_t*	table);	/* in: database table in dictionary cache */
/*****************************************************************
Removes a granted record lock of a transaction from the queue and grants
locks to other transactions waiting in the queue if they now are entitled
to a lock. */
UNIV_INTERN
void
lock_rec_unlock(
/*============*/
	trx_t*			trx,	/* in: transaction that has
					set a record lock */
	const buf_block_t*	block,	/* in: buffer block containing rec */
	const rec_t*		rec,	/* in: record */
	enum lock_mode		lock_mode);/* in: LOCK_S or LOCK_X */
/*************************************************************************
Releases a table lock.
Releases possible other transactions waiting for this lock. */
UNIV_INTERN
void
lock_table_unlock(
/*==============*/
	lock_t*	lock);	/* in: lock */
/*************************************************************************
Releases an auto-inc lock a transaction possibly has on a table.
Releases possible other transactions waiting for this lock. */
UNIV_INTERN
void
lock_table_unlock_auto_inc(
/*=======================*/
	trx_t*	trx);	/* in: transaction */
/*************************************************************************
Releases transaction locks, and releases possible other transactions waiting
because of these locks. */
UNIV_INTERN
void
lock_release_off_kernel(
/*====================*/
	trx_t*	trx);	/* in: transaction */
/*************************************************************************
Cancels a waiting lock request and releases possible other transactions
waiting behind it. */
UNIV_INTERN
void
lock_cancel_waiting_and_release(
/*============================*/
	lock_t*	lock);	/* in: waiting lock request */
/*************************************************************************
Resets all locks, both table and record locks, on a table to be dropped.
No lock is allowed to be a wait lock. */
UNIV_INTERN
void
lock_reset_all_on_table(
/*====================*/
	dict_table_t*	table);	/* in: table to be dropped */
/*************************************************************************
Calculates the fold value of a page file address: used in inserting or
searching for a lock in the hash table. */
UNIV_INLINE
ulint
lock_rec_fold(
/*==========*/
			/* out: folded value */
	ulint	space,	/* in: space */
	ulint	page_no)/* in: page number */
	__attribute__((const));
/*************************************************************************
Calculates the hash value of a page file address: used in inserting or
searching for a lock in the hash table. */
UNIV_INLINE
ulint
lock_rec_hash(
/*==========*/
			/* out: hashed value */
	ulint	space,	/* in: space */
	ulint	page_no);/* in: page number */

/**************************************************************************
Looks for a set bit in a record lock bitmap. Returns ULINT_UNDEFINED,
if none found. */
UNIV_INTERN
ulint
lock_rec_find_set_bit(
/*==================*/
				/* out: bit index == heap number of
				the record, or ULINT_UNDEFINED if none found */
	const lock_t*	lock);	/* in: record lock with at least one bit set */

/*************************************************************************
Gets the source table of an ALTER TABLE transaction.  The table must be
covered by an IX or IS table lock. */
UNIV_INTERN
dict_table_t*
lock_get_src_table(
/*===============*/
				/* out: the source table of transaction,
				if it is covered by an IX or IS table lock;
				dest if there is no source table, and
				NULL if the transaction is locking more than
				two tables or an inconsistency is found */
	trx_t*		trx,	/* in: transaction */
	dict_table_t*	dest,	/* in: destination of ALTER TABLE */
	enum lock_mode*	mode);	/* out: lock mode of the source table */
/*************************************************************************
Determine if the given table is exclusively "owned" by the given
transaction, i.e., transaction holds LOCK_IX and possibly LOCK_AUTO_INC
on the table. */
UNIV_INTERN
ibool
lock_is_table_exclusive(
/*====================*/
				/* out: TRUE if table is only locked by trx,
				with LOCK_IX, and possibly LOCK_AUTO_INC */
	dict_table_t*	table,	/* in: table */
	trx_t*		trx);	/* in: transaction */
/*************************************************************************
Checks if a lock request lock1 has to wait for request lock2. */
UNIV_INTERN
ibool
lock_has_to_wait(
/*=============*/
				/* out: TRUE if lock1 has to wait for
				lock2 to be removed */
	const lock_t*	lock1,	/* in: waiting lock */
	const lock_t*	lock2);	/* in: another lock; NOTE that it is
				assumed that this has a lock bit set
				on the same record as in lock1 if the
				locks are record locks */
/*************************************************************************
Checks that a transaction id is sensible, i.e., not in the future. */
UNIV_INTERN
ibool
lock_check_trx_id_sanity(
/*=====================*/
					/* out: TRUE if ok */
	dulint		trx_id,		/* in: trx id */
	const rec_t*	rec,		/* in: user record */
	dict_index_t*	index,		/* in: clustered index */
	const ulint*	offsets,	/* in: rec_get_offsets(rec, index) */
	ibool		has_kernel_mutex);/* in: TRUE if the caller owns the
					kernel mutex */
/*************************************************************************
Prints info of a table lock. */
UNIV_INTERN
void
lock_table_print(
/*=============*/
	FILE*		file,	/* in: file where to print */
	const lock_t*	lock);	/* in: table type lock */
/*************************************************************************
Prints info of a record lock. */
UNIV_INTERN
void
lock_rec_print(
/*===========*/
	FILE*		file,	/* in: file where to print */
	const lock_t*	lock);	/* in: record type lock */
/*************************************************************************
Prints info of locks for all transactions. */
UNIV_INTERN
void
lock_print_info_summary(
/*====================*/
	FILE*	file);	/* in: file where to print */
/*************************************************************************
Prints info of locks for each transaction. */
UNIV_INTERN
void
lock_print_info_all_transactions(
/*=============================*/
	FILE*	file);	/* in: file where to print */
/*************************************************************************
Return approximate number or record locks (bits set in the bitmap) for
this transaction. Since delete-marked records may be removed, the
record count will not be precise. */
UNIV_INTERN
ulint
lock_number_of_rows_locked(
/*=======================*/
	trx_t*	trx);	/* in: transaction */

/***********************************************************************
Gets the type of a lock. Non-inline version for using outside of the
lock module. */
UNIV_INTERN
ulint
lock_get_type(
/*==========*/
				/* out: LOCK_TABLE or LOCK_REC */
	const lock_t*	lock);	/* in: lock */

/***********************************************************************
Gets the id of the transaction owning a lock. */
UNIV_INTERN
ullint
lock_get_trx_id(
/*============*/
				/* out: transaction id */
	const lock_t*	lock);	/* in: lock */

/***********************************************************************
Gets the mode of a lock in a human readable string.
The string should not be free()'d or modified. */

const char*
lock_get_mode_str(
/*==============*/
				/* out: lock mode */
	const lock_t*	lock);	/* in: lock */

/***********************************************************************
Gets the type of a lock in a human readable string.
The string should not be free()'d or modified. */

const char*
lock_get_type_str(
/*==============*/
				/* out: lock type */
	const lock_t*	lock);	/* in: lock */

/***********************************************************************
Gets the id of the table on which the lock is. */
UNIV_INTERN
ullint
lock_get_table_id(
/*==============*/
				/* out: id of the table */
	const lock_t*	lock);	/* in: lock */

/***********************************************************************
Gets the name of the table on which the lock is.
The string should not be free()'d or modified. */

const char*
lock_get_table_name(
/*================*/
				/* out: name of the table */
	const lock_t*	lock);	/* in: lock */

/***********************************************************************
For a record lock, gets the index on which the lock is. */

const dict_index_t*
lock_rec_get_index(
/*===============*/
				/* out: index */
	const lock_t*	lock);	/* in: lock */

/***********************************************************************
For a record lock, gets the name of the index on which the lock is.
The string should not be free()'d or modified. */

const char*
lock_rec_get_index_name(
/*====================*/
				/* out: name of the index */
	const lock_t*	lock);	/* in: lock */

/***********************************************************************
For a record lock, gets the tablespace number on which the lock is. */
UNIV_INTERN
ulint
lock_rec_get_space_id(
/*==================*/
				/* out: tablespace number */
	const lock_t*	lock);	/* in: lock */

/***********************************************************************
For a record lock, gets the page number on which the lock is. */
UNIV_INTERN
ulint
lock_rec_get_page_no(
/*=================*/
				/* out: page number */
	const lock_t*	lock);	/* in: lock */

/* Lock modes and types */
#define LOCK_MODE_MASK	0xFUL	/* mask used to extract mode from the
				type_mode field in a lock */
/* Lock types */
#define LOCK_TABLE	16	/* these type values should be so high that */
#define	LOCK_REC	32	/* they can be ORed to the lock mode */
#define LOCK_TYPE_MASK	0xF0UL	/* mask used to extract lock type from the
				type_mode field in a lock */
/* Waiting lock flag */
#define LOCK_WAIT	256	/* this wait bit should be so high that
				it can be ORed to the lock mode and type;
				when this bit is set, it means that the
				lock has not yet been granted, it is just
				waiting for its turn in the wait queue */
/* Precise modes */
#define LOCK_ORDINARY	0	/* this flag denotes an ordinary next-key lock
				in contrast to LOCK_GAP or LOCK_REC_NOT_GAP */
#define LOCK_GAP	512	/* this gap bit should be so high that
				it can be ORed to the other flags;
				when this bit is set, it means that the
				lock holds only on the gap before the record;
				for instance, an x-lock on the gap does not
				give permission to modify the record on which
				the bit is set; locks of this type are created
				when records are removed from the index chain
				of records */
#define LOCK_REC_NOT_GAP 1024	/* this bit means that the lock is only on
				the index record and does NOT block inserts
				to the gap before the index record; this is
				used in the case when we retrieve a record
				with a unique key, and is also used in
				locking plain SELECTs (not part of UPDATE
				or DELETE) when the user has set the READ
				COMMITTED isolation level */
#define LOCK_INSERT_INTENTION 2048 /* this bit is set when we place a waiting
				gap type record lock request in order to let
				an insert of an index record to wait until
				there are no conflicting locks by other
				transactions on the gap; note that this flag
				remains set when the waiting lock is granted,
				or if the lock is inherited to a neighboring
				record */

/* When lock bits are reset, the following flags are available: */
#define LOCK_RELEASE_WAIT	1
#define LOCK_NOT_RELEASE_WAIT	2

/* Lock operation struct */
typedef struct lock_op_struct	lock_op_t;
struct lock_op_struct{
	dict_table_t*	table;	/* table to be locked */
	enum lock_mode	mode;	/* lock mode */
};

#define LOCK_OP_START		1
#define LOCK_OP_COMPLETE	2

/* The lock system struct */
struct lock_sys_struct{
	hash_table_t*	rec_hash;	/* hash table of the record locks */
};

/* The lock system */
extern lock_sys_t*	lock_sys;


#ifndef UNIV_NONINL
#include "lock0lock.ic"
#endif

#endif
