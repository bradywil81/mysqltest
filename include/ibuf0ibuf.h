/******************************************************
Insert buffer

(c) 1997 Innobase Oy

Created 7/19/1997 Heikki Tuuri
*******************************************************/

#ifndef ibuf0ibuf_h
#define ibuf0ibuf_h

#include "univ.i"

#include "dict0mem.h"
#include "dict0dict.h"
#include "mtr0mtr.h"
#include "que0types.h"
#include "ibuf0types.h"
#include "fsp0fsp.h"

extern ibuf_t*	ibuf;

/**********************************************************************
Creates the insert buffer data struct for a single tablespace. Reads the
root page of the insert buffer tree in the tablespace. This function can
be called only after the dictionary system has been initialized, as this
creates also the insert buffer table and index for this tablespace. */
UNIV_INTERN
ibuf_data_t*
ibuf_data_init_for_space(
/*=====================*/
			/* out, own: ibuf data struct, linked to the list
			in ibuf control structure. */
	ulint	space);	/* in: space id */
/**********************************************************************
Creates the insert buffer data structure at a database startup and
initializes the data structures for the insert buffer of each tablespace. */
UNIV_INTERN
void
ibuf_init_at_db_start(void);
/*=======================*/
/*************************************************************************
Reads the biggest tablespace id from the high end of the insert buffer
tree and updates the counter in fil_system. */
UNIV_INTERN
void
ibuf_update_max_tablespace_id(void);
/*===============================*/
/*************************************************************************
Initializes an ibuf bitmap page. */
UNIV_INTERN
void
ibuf_bitmap_page_init(
/*==================*/
	buf_block_t*	block,	/* in: bitmap page */
	mtr_t*		mtr);	/* in: mtr */
/****************************************************************************
Resets the free bits of the page in the ibuf bitmap. This is done in a
separate mini-transaction, hence this operation does not restrict further
work to only ibuf bitmap operations, which would result if the latch to the
bitmap page were kept. */
UNIV_INTERN
void
ibuf_reset_free_bits(
/*=================*/
	buf_block_t*	block);	/* in: index page; free bits are set to 0
				if the index is a non-clustered
				non-unique, and page level is 0 */
/****************************************************************************
Updates the free bits of an uncompressed page in the ibuf bitmap if
there is not enough free on the page any more. This is done in a
separate mini-transaction, hence this operation does not restrict
further work to only ibuf bitmap operations, which would result if the
latch to the bitmap page were kept. */
UNIV_INLINE
void
ibuf_update_free_bits_if_full(
/*==========================*/
	buf_block_t*	block,	/* in: index page to which we have added new
				records; the free bits are updated if the
				index is non-clustered and non-unique and
				the page level is 0, and the page becomes
				fuller */
	ulint		max_ins_size,/* in: value of maximum insert size with
				reorganize before the latest operation
				performed to the page */
	ulint		increase);/* in: upper limit for the additional space
				used in the latest operation, if known, or
				ULINT_UNDEFINED */
/**************************************************************************
Updates the free bits for an uncompressed page to reflect the present state.
Does this in the mtr given, which means that the latching order rules virtually
prevent any further operations for this OS thread until mtr is committed. */
UNIV_INTERN
void
ibuf_update_free_bits_low(
/*======================*/
	const buf_block_t*	block,		/* in: index page */
	ulint			max_ins_size,	/* in: value of
						maximum insert size
						with reorganize before
						the latest operation
						performed to the page */
	mtr_t*			mtr);		/* in/out: mtr */
/**************************************************************************
Updates the free bits for a compressed page to reflect the present state.
Does this in the mtr given, which means that the latching order rules virtually
prevent any further operations for this OS thread until mtr is committed. */
UNIV_INTERN
void
ibuf_update_free_bits_zip(
/*======================*/
	const buf_block_t*	block,	/* in: index page */
	mtr_t*			mtr);	/* in/out: mtr */
/**************************************************************************
Updates the free bits for the two pages to reflect the present state. Does
this in the mtr given, which means that the latching order rules virtually
prevent any further operations until mtr is committed. */
UNIV_INTERN
void
ibuf_update_free_bits_for_two_pages_low(
/*====================================*/
	ulint		zip_size,/* in: compressed page size in bytes;
				0 for uncompressed pages */
	buf_block_t*	block1,	/* in: index page */
	buf_block_t*	block2,	/* in: index page */
	mtr_t*		mtr);	/* in: mtr */
/**************************************************************************
A basic partial test if an insert to the insert buffer could be possible and
recommended. */
UNIV_INLINE
ibool
ibuf_should_try(
/*============*/
	dict_index_t*	index,			/* in: index where to insert */
	ulint		ignore_sec_unique);	/* in: if != 0, we should
						ignore UNIQUE constraint on
						a secondary index when we
						decide */
/**********************************************************************
Returns TRUE if the current OS thread is performing an insert buffer
routine. */
UNIV_INTERN
ibool
ibuf_inside(void);
/*=============*/
		/* out: TRUE if inside an insert buffer routine: for instance,
		a read-ahead of non-ibuf pages is then forbidden */
/***************************************************************************
Checks if a page address is an ibuf bitmap page (level 3 page) address. */
UNIV_INLINE
ibool
ibuf_bitmap_page(
/*=============*/
			/* out: TRUE if a bitmap page */
	ulint	zip_size,/* in: compressed page size in bytes;
			0 for uncompressed pages */
	ulint	page_no);/* in: page number */
/***************************************************************************
Checks if a page is a level 2 or 3 page in the ibuf hierarchy of pages. */
UNIV_INTERN
ibool
ibuf_page(
/*======*/
			/* out: TRUE if level 2 or level 3 page */
	ulint	space,	/* in: space id */
	ulint	zip_size,/* in: compressed page size in bytes, or 0 */
	ulint	page_no);/* in: page number */
/***************************************************************************
Checks if a page is a level 2 or 3 page in the ibuf hierarchy of pages. */
UNIV_INTERN
ibool
ibuf_page_low(
/*==========*/
			/* out: TRUE if level 2 or level 3 page */
	ulint	space,	/* in: space id */
	ulint	zip_size,/* in: compressed page size in bytes, or 0 */
	ulint	page_no,/* in: page number */
	mtr_t*	mtr);	/* in: mtr which will contain an x-latch to the
			bitmap page if the page is not one of the fixed
			address ibuf pages */
/***************************************************************************
Frees excess pages from the ibuf free list. This function is called when an OS
thread calls fsp services to allocate a new file segment, or a new page to a
file segment, and the thread did not own the fsp latch before this call. */
UNIV_INTERN
void
ibuf_free_excess_pages(
/*===================*/
	ulint	space);		/* in: space id */
/*************************************************************************
Makes an index insert to the insert buffer, instead of directly to the disk
page, if this is possible. Does not do insert if the index is clustered
or unique. */
UNIV_INTERN
ibool
ibuf_insert(
/*========*/
				/* out: TRUE if success */
	const dtuple_t*	entry,	/* in: index entry to insert */
	dict_index_t*	index,	/* in: index where to insert */
	ulint		space,	/* in: space id where to insert */
	ulint		zip_size,/* in: compressed page size in bytes, or 0 */
	ulint		page_no,/* in: page number where to insert */
	que_thr_t*	thr);	/* in: query thread */
/*************************************************************************
When an index page is read from a disk to the buffer pool, this function
inserts to the page the possible index entries buffered in the insert buffer.
The entries are deleted from the insert buffer. If the page is not read, but
created in the buffer pool, this function deletes its buffered entries from
the insert buffer; there can exist entries for such a page if the page
belonged to an index which subsequently was dropped. */
UNIV_INTERN
void
ibuf_merge_or_delete_for_page(
/*==========================*/
	buf_block_t*	block,	/* in: if page has been read from
				disk, pointer to the page x-latched,
				else NULL */
	ulint		space,	/* in: space id of the index page */
	ulint		page_no,/* in: page number of the index page */
	ulint		zip_size,/* in: compressed page size in bytes,
				or 0 */
	ibool		update_ibuf_bitmap);/* in: normally this is set
				to TRUE, but if we have deleted or are
				deleting the tablespace, then we
				naturally do not want to update a
				non-existent bitmap page */
/*************************************************************************
Deletes all entries in the insert buffer for a given space id. This is used
in DISCARD TABLESPACE and IMPORT TABLESPACE.
NOTE: this does not update the page free bitmaps in the space. The space will
become CORRUPT when you call this function! */
UNIV_INTERN
void
ibuf_delete_for_discarded_space(
/*============================*/
	ulint	space);	/* in: space id */
/*************************************************************************
Contracts insert buffer trees by reading pages to the buffer pool. */
UNIV_INTERN
ulint
ibuf_contract(
/*==========*/
			/* out: a lower limit for the combined size in bytes
			of entries which will be merged from ibuf trees to the
			pages read, 0 if ibuf is empty */
	ibool	sync);	/* in: TRUE if the caller wants to wait for the
			issued read with the highest tablespace address
			to complete */
/*************************************************************************
Contracts insert buffer trees by reading pages to the buffer pool. */
UNIV_INTERN
ulint
ibuf_contract_for_n_pages(
/*======================*/
			/* out: a lower limit for the combined size in bytes
			of entries which will be merged from ibuf trees to the
			pages read, 0 if ibuf is empty */
	ibool	sync,	/* in: TRUE if the caller wants to wait for the
			issued read with the highest tablespace address
			to complete */
	ulint	n_pages);/* in: try to read at least this many pages to
			the buffer pool and merge the ibuf contents to
			them */
/*************************************************************************
Parses a redo log record of an ibuf bitmap page init. */
UNIV_INTERN
byte*
ibuf_parse_bitmap_init(
/*===================*/
				/* out: end of log record or NULL */
	byte*		ptr,	/* in: buffer */
	byte*		end_ptr,/* in: buffer end */
	buf_block_t*	block,	/* in: block or NULL */
	mtr_t*		mtr);	/* in: mtr or NULL */
#ifdef UNIV_IBUF_COUNT_DEBUG
/**********************************************************************
Gets the ibuf count for a given page. */
UNIV_INTERN
ulint
ibuf_count_get(
/*===========*/
			/* out: number of entries in the insert buffer
			currently buffered for this page */
	ulint	space,	/* in: space id */
	ulint	page_no);/* in: page number */
#endif
/**********************************************************************
Looks if the insert buffer is empty. */
UNIV_INTERN
ibool
ibuf_is_empty(void);
/*===============*/
			/* out: TRUE if empty */
/**********************************************************************
Prints info of ibuf. */
UNIV_INTERN
void
ibuf_print(
/*=======*/
	FILE*	file);	/* in: file where to print */

#define IBUF_HEADER_PAGE_NO	FSP_IBUF_HEADER_PAGE_NO
#define IBUF_TREE_ROOT_PAGE_NO	FSP_IBUF_TREE_ROOT_PAGE_NO

/* The ibuf header page currently contains only the file segment header
for the file segment from which the pages for the ibuf tree are allocated */
#define IBUF_HEADER		PAGE_DATA
#define	IBUF_TREE_SEG_HEADER	0	/* fseg header for ibuf tree */

#ifndef UNIV_NONINL
#include "ibuf0ibuf.ic"
#endif

#endif
