/******************************************************
The database buffer replacement algorithm

(c) 1995 Innobase Oy

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#include "buf0lru.h"

#ifdef UNIV_NONINL
#include "buf0lru.ic"
#endif

#include "ut0byte.h"
#include "ut0lst.h"
#include "ut0rnd.h"
#include "sync0sync.h"
#include "sync0rw.h"
#include "hash0hash.h"
#include "os0sync.h"
#include "fil0fil.h"
#include "btr0btr.h"
#include "buf0buddy.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include "btr0sea.h"
#include "os0file.h"
#include "page0zip.h"
#include "log0recv.h"
#include "srv0srv.h"

/* The number of blocks from the LRU_old pointer onward, including the block
pointed to, must be 3/8 of the whole LRU list length, except that the
tolerance defined below is allowed. Note that the tolerance must be small
enough such that for even the BUF_LRU_OLD_MIN_LEN long LRU list, the
LRU_old pointer is not allowed to point to either end of the LRU list. */

#define BUF_LRU_OLD_TOLERANCE	20

/* The whole LRU list length is divided by this number to determine an
initial segment in buf_LRU_get_recent_limit */

#define BUF_LRU_INITIAL_RATIO	8

/* If we switch on the InnoDB monitor because there are too few available
frames in the buffer pool, we set this to TRUE */
UNIV_INTERN ibool	buf_lru_switched_on_innodb_mon	= FALSE;

/**********************************************************************
Takes a block out of the LRU list and page hash table.
If the block is compressed-only (BUF_BLOCK_ZIP_PAGE),
the object will be freed and buf_pool_zip_mutex will be released.

If a compressed page or a compressed-only block descriptor is freed,
other compressed pages or compressed-only block descriptors may be
relocated. */
static
enum buf_page_state
buf_LRU_block_remove_hashed_page(
/*=============================*/
				/* out: the new state of the block
				(BUF_BLOCK_ZIP_FREE if the state was
				BUF_BLOCK_ZIP_PAGE, or BUF_BLOCK_REMOVE_HASH
				otherwise) */
	buf_page_t*	bpage,	/* in: block, must contain a file page and
				be in a state where it can be freed; there
				may or may not be a hash index to the page */
	ibool		zip);	/* in: TRUE if should remove also the
				compressed page of an uncompressed page */
/**********************************************************************
Puts a file page whose has no hash index to the free list. */
static
void
buf_LRU_block_free_hashed_page(
/*===========================*/
	buf_block_t*	block);	/* in: block, must contain a file page and
				be in a state where it can be freed */

/**********************************************************************
Invalidates all pages belonging to a given tablespace when we are deleting
the data file(s) of that tablespace. */
UNIV_INTERN
void
buf_LRU_invalidate_tablespace(
/*==========================*/
	ulint	id)	/* in: space id */
{
	buf_page_t*	bpage;
	ulint		page_no;
	ibool		all_freed;

scan_again:
	buf_pool_mutex_enter();

	all_freed = TRUE;

	bpage = UT_LIST_GET_LAST(buf_pool->LRU);

	while (bpage != NULL) {
		mutex_t*	block_mutex = buf_page_get_mutex(bpage);
		buf_page_t*	prev_bpage;

		ut_a(buf_page_in_file(bpage));

		mutex_enter(block_mutex);
		prev_bpage = UT_LIST_GET_PREV(LRU, bpage);

		if (buf_page_get_space(bpage) == id) {
			if (bpage->buf_fix_count > 0
			    || buf_page_get_io_fix(bpage) != BUF_IO_NONE) {

				/* We cannot remove this page during
				this scan yet; maybe the system is
				currently reading it in, or flushing
				the modifications to the file */

				all_freed = FALSE;

				goto next_page;
			}

#ifdef UNIV_DEBUG
			if (buf_debug_prints) {
				fprintf(stderr,
					"Dropping space %lu page %lu\n",
					(ulong) buf_page_get_space(bpage),
					(ulong) buf_page_get_page_no(bpage));
			}
#endif
			if (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE
			    && ((buf_block_t*) bpage)->is_hashed) {
				page_no = buf_page_get_page_no(bpage);

				buf_pool_mutex_exit();
				mutex_exit(block_mutex);

				/* Note that the following call will acquire
				an S-latch on the page */

				btr_search_drop_page_hash_when_freed(
					id,
					buf_page_get_zip_size(bpage),
					page_no);
				goto scan_again;
			}

			if (bpage->oldest_modification != 0) {

				buf_flush_remove(bpage);
			}

			/* Remove from the LRU list */
			if (buf_LRU_block_remove_hashed_page(bpage, TRUE)
			    != BUF_BLOCK_ZIP_FREE) {
				buf_LRU_block_free_hashed_page((buf_block_t*)
							       bpage);
			} else {
				/* The compressed block descriptor
				(bpage) has been deallocated and
				block_mutex released.  Also,
				buf_buddy_free() may have relocated
				prev_bpage.  Rescan the LRU list. */

				bpage = UT_LIST_GET_LAST(buf_pool->LRU);
				continue;
			}
		}
next_page:
		mutex_exit(block_mutex);
		bpage = prev_bpage;
	}

	buf_pool_mutex_exit();

	if (!all_freed) {
		os_thread_sleep(20000);

		goto scan_again;
	}
}

/**********************************************************************
Gets the minimum LRU_position field for the blocks in an initial segment
(determined by BUF_LRU_INITIAL_RATIO) of the LRU list. The limit is not
guaranteed to be precise, because the ulint_clock may wrap around. */
UNIV_INTERN
ulint
buf_LRU_get_recent_limit(void)
/*==========================*/
			/* out: the limit; zero if could not determine it */
{
	const buf_page_t*	bpage;
	ulint			len;
	ulint			limit;

	buf_pool_mutex_enter();

	len = UT_LIST_GET_LEN(buf_pool->LRU);

	if (len < BUF_LRU_OLD_MIN_LEN) {
		/* The LRU list is too short to do read-ahead */

		buf_pool_mutex_exit();

		return(0);
	}

	bpage = UT_LIST_GET_FIRST(buf_pool->LRU);

	limit = buf_page_get_LRU_position(bpage) - len / BUF_LRU_INITIAL_RATIO;

	buf_pool_mutex_exit();

	return(limit);
}

/************************************************************************
Insert a compressed block into buf_pool->zip_clean in the LRU order. */
UNIV_INTERN
void
buf_LRU_insert_zip_clean(
/*=====================*/
	buf_page_t*	bpage)	/* in: pointer to the block in question */
{
	buf_page_t*	b;

	ut_ad(buf_pool_mutex_own());
	ut_ad(buf_page_get_state(bpage) == BUF_BLOCK_ZIP_PAGE);

	/* Find the first successor of bpage in the LRU list
	that is in the zip_clean list. */
	b = bpage;
	do {
		b = UT_LIST_GET_NEXT(LRU, b);
	} while (b && buf_page_get_state(b) != BUF_BLOCK_ZIP_PAGE);

	/* Insert bpage before b, i.e., after the predecessor of b. */
	if (b) {
		b = UT_LIST_GET_PREV(list, b);
	}

	if (b) {
		UT_LIST_INSERT_AFTER(list, buf_pool->zip_clean, b, bpage);
	} else {
		UT_LIST_ADD_FIRST(list, buf_pool->zip_clean, bpage);
	}
}

/**********************************************************************
Look for a replaceable block from the end of the LRU list and put it to
the free list if found. */
UNIV_INTERN
ibool
buf_LRU_search_and_free_block(
/*==========================*/
				/* out: TRUE if freed */
	ulint	n_iterations)	/* in: how many times this has been called
				repeatedly without result: a high value means
				that we should search farther; if value is
				k < 10, then we only search k/10 * [number
				of pages in the buffer pool] from the end
				of the LRU list */
{
	buf_page_t*	bpage;
	ibool		freed;

	buf_pool_mutex_enter();

	freed = FALSE;
	bpage = UT_LIST_GET_LAST(buf_pool->LRU);

	if (UNIV_UNLIKELY(n_iterations > 10)) {
		/* The buffer pool is scarce.  Search the whole LRU list. */

		while (bpage != NULL) {
			mutex_t*	block_mutex
				= buf_page_get_mutex(bpage);

			mutex_enter(block_mutex);
			freed = buf_LRU_free_block(bpage, TRUE, NULL);
			mutex_exit(block_mutex);

			if (freed) {

				break;
			}

			bpage = UT_LIST_GET_PREV(LRU, bpage);
		}
	} else if (buf_buddy_n_frames > buf_buddy_min_n_frames) {
		/* There are enough compressed blocks.  Free the
		least recently used block, whether or not it
		comprises an uncompressed page. */

		ulint	distance = 100
			+ (n_iterations * buf_pool->curr_size) / 10;

		while (bpage != NULL) {
			mutex_t*	block_mutex
				= buf_page_get_mutex(bpage);

			mutex_enter(block_mutex);
			freed = buf_LRU_free_block(bpage, TRUE, NULL);
			mutex_exit(block_mutex);

			if (freed) {

				break;
			}

			bpage = UT_LIST_GET_PREV(LRU, bpage);

			if (!--distance) {
				goto func_exit;
			}
		}
	} else {
		/* There are few compressed blocks.  Skip compressed-only
		blocks in the search for the least recently used block
		that can be freed. */

		ulint	distance = 100
			+ (n_iterations * buf_pool->curr_size) / 10;

		while (bpage != NULL) {
			if (buf_page_get_state(bpage)
			    == BUF_BLOCK_FILE_PAGE) {

				buf_block_t*	block = (buf_block_t*) bpage;
				mutex_enter(&block->mutex);
				freed = buf_LRU_free_block(bpage, TRUE, NULL);
				mutex_exit(&block->mutex);

				if (freed) {

					break;
				}
			}

			bpage = UT_LIST_GET_PREV(LRU, bpage);

			if (!--distance) {
				goto func_exit;
			}
		}
	}

	if (buf_pool->LRU_flush_ended > 0) {
		buf_pool->LRU_flush_ended--;
	}

func_exit:
	if (!freed) {
		buf_pool->LRU_flush_ended = 0;
	}
	buf_pool_mutex_exit();

	return(freed);
}

/**********************************************************************
Tries to remove LRU flushed blocks from the end of the LRU list and put them
to the free list. This is beneficial for the efficiency of the insert buffer
operation, as flushed pages from non-unique non-clustered indexes are here
taken out of the buffer pool, and their inserts redirected to the insert
buffer. Otherwise, the flushed blocks could get modified again before read
operations need new buffer blocks, and the i/o work done in flushing would be
wasted. */
UNIV_INTERN
void
buf_LRU_try_free_flushed_blocks(void)
/*=================================*/
{
	buf_pool_mutex_enter();

	while (buf_pool->LRU_flush_ended > 0) {

		buf_pool_mutex_exit();

		buf_LRU_search_and_free_block(1);

		buf_pool_mutex_enter();
	}

	buf_pool_mutex_exit();
}

/**********************************************************************
Returns TRUE if less than 25 % of the buffer pool is available. This can be
used in heuristics to prevent huge transactions eating up the whole buffer
pool for their locks. */
UNIV_INTERN
ibool
buf_LRU_buf_pool_running_out(void)
/*==============================*/
				/* out: TRUE if less than 25 % of buffer pool
				left */
{
	ibool	ret	= FALSE;

	buf_pool_mutex_enter();

	if (!recv_recovery_on && UT_LIST_GET_LEN(buf_pool->free)
	    + UT_LIST_GET_LEN(buf_pool->LRU) < buf_pool->curr_size / 4) {

		ret = TRUE;
	}

	buf_pool_mutex_exit();

	return(ret);
}

/**********************************************************************
Returns a free block from the buf_pool.  The block is taken off the
free list.  If it is empty, returns NULL. */
UNIV_INTERN
buf_block_t*
buf_LRU_get_free_only(void)
/*=======================*/
				/* out: a free control block, or NULL
				if the buf_block->free list is empty */
{
	buf_block_t*	block;

	ut_ad(buf_pool_mutex_own());

	block = (buf_block_t*) UT_LIST_GET_FIRST(buf_pool->free);

	if (block) {
		ut_ad(block->page.in_free_list);
		ut_d(block->page.in_free_list = FALSE);
		ut_ad(!block->page.in_flush_list);
		ut_ad(!block->page.in_LRU_list);
		ut_a(!buf_page_in_file(&block->page));
		UT_LIST_REMOVE(list, buf_pool->free, (&block->page));

		mutex_enter(&block->mutex);

		buf_block_set_state(block, BUF_BLOCK_READY_FOR_USE);
		UNIV_MEM_ALLOC(block->frame, UNIV_PAGE_SIZE);

		mutex_exit(&block->mutex);
	}

	return(block);
}

/**********************************************************************
Returns a free block from the buf_pool. The block is taken off the
free list. If it is empty, blocks are moved from the end of the
LRU list to the free list. */
UNIV_INTERN
buf_block_t*
buf_LRU_get_free_block(
/*===================*/
				/* out: the free control block,
				in state BUF_BLOCK_READY_FOR_USE */
	ulint	zip_size)	/* in: compressed page size in bytes,
				or 0 if uncompressed tablespace */
{
	buf_block_t*	block		= NULL;
	ibool		freed;
	ulint		n_iterations	= 1;
	ibool		mon_value_was	= FALSE;
	ibool		started_monitor	= FALSE;
loop:
	buf_pool_mutex_enter();

	if (!recv_recovery_on && UT_LIST_GET_LEN(buf_pool->free)
	    + UT_LIST_GET_LEN(buf_pool->LRU) < buf_pool->curr_size / 20) {
		ut_print_timestamp(stderr);

		fprintf(stderr,
			"  InnoDB: ERROR: over 95 percent of the buffer pool"
			" is occupied by\n"
			"InnoDB: lock heaps or the adaptive hash index!"
			" Check that your\n"
			"InnoDB: transactions do not set too many row locks.\n"
			"InnoDB: Your buffer pool size is %lu MB."
			" Maybe you should make\n"
			"InnoDB: the buffer pool bigger?\n"
			"InnoDB: We intentionally generate a seg fault"
			" to print a stack trace\n"
			"InnoDB: on Linux!\n",
			(ulong) (buf_pool->curr_size
				 / (1024 * 1024 / UNIV_PAGE_SIZE)));

		ut_error;

	} else if (!recv_recovery_on
		   && (UT_LIST_GET_LEN(buf_pool->free)
		       + UT_LIST_GET_LEN(buf_pool->LRU))
		   < buf_pool->curr_size / 3) {

		if (!buf_lru_switched_on_innodb_mon) {

	   		/* Over 67 % of the buffer pool is occupied by lock
			heaps or the adaptive hash index. This may be a memory
			leak! */

			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: WARNING: over 67 percent of"
				" the buffer pool is occupied by\n"
				"InnoDB: lock heaps or the adaptive"
				" hash index! Check that your\n"
				"InnoDB: transactions do not set too many"
				" row locks.\n"
				"InnoDB: Your buffer pool size is %lu MB."
				" Maybe you should make\n"
				"InnoDB: the buffer pool bigger?\n"
				"InnoDB: Starting the InnoDB Monitor to print"
				" diagnostics, including\n"
				"InnoDB: lock heap and hash index sizes.\n",
				(ulong) (buf_pool->curr_size
					 / (1024 * 1024 / UNIV_PAGE_SIZE)));

			buf_lru_switched_on_innodb_mon = TRUE;
			srv_print_innodb_monitor = TRUE;
			os_event_set(srv_lock_timeout_thread_event);
		}
	} else if (buf_lru_switched_on_innodb_mon) {

		/* Switch off the InnoDB Monitor; this is a simple way
		to stop the monitor if the situation becomes less urgent,
		but may also surprise users if the user also switched on the
		monitor! */

		buf_lru_switched_on_innodb_mon = FALSE;
		srv_print_innodb_monitor = FALSE;
	}

	/* If there is a block in the free list, take it */
	block = buf_LRU_get_free_only();
	if (block) {

#ifdef UNIV_DEBUG
		block->page.zip.m_start =
#endif /* UNIV_DEBUG */
			block->page.zip.m_end =
			block->page.zip.m_nonempty =
			block->page.zip.n_blobs = 0;

		if (UNIV_UNLIKELY(zip_size)) {
			ibool	lru;
			page_zip_set_size(&block->page.zip, zip_size);
			block->page.zip.data = buf_buddy_alloc(zip_size, &lru);
			UNIV_MEM_DESC(block->page.zip.data, zip_size, block);
		} else {
			page_zip_set_size(&block->page.zip, 0);
			block->page.zip.data = NULL;
		}

		buf_pool_mutex_exit();

		if (started_monitor) {
			srv_print_innodb_monitor = mon_value_was;
		}

		return(block);
	}

	/* If no block was in the free list, search from the end of the LRU
	list and try to free a block there */

	buf_pool_mutex_exit();

	freed = buf_LRU_search_and_free_block(n_iterations);

	if (freed > 0) {
		goto loop;
	}

	if (n_iterations > 30) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"InnoDB: Warning: difficult to find free blocks from\n"
			"InnoDB: the buffer pool (%lu search iterations)!"
			" Consider\n"
			"InnoDB: increasing the buffer pool size.\n"
			"InnoDB: It is also possible that"
			" in your Unix version\n"
			"InnoDB: fsync is very slow, or"
			" completely frozen inside\n"
			"InnoDB: the OS kernel. Then upgrading to"
			" a newer version\n"
			"InnoDB: of your operating system may help."
			" Look at the\n"
			"InnoDB: number of fsyncs in diagnostic info below.\n"
			"InnoDB: Pending flushes (fsync) log: %lu;"
			" buffer pool: %lu\n"
			"InnoDB: %lu OS file reads, %lu OS file writes,"
			" %lu OS fsyncs\n"
			"InnoDB: Starting InnoDB Monitor to print further\n"
			"InnoDB: diagnostics to the standard output.\n",
			(ulong) n_iterations,
			(ulong) fil_n_pending_log_flushes,
			(ulong) fil_n_pending_tablespace_flushes,
			(ulong) os_n_file_reads, (ulong) os_n_file_writes,
			(ulong) os_n_fsyncs);

		mon_value_was = srv_print_innodb_monitor;
		started_monitor = TRUE;
		srv_print_innodb_monitor = TRUE;
		os_event_set(srv_lock_timeout_thread_event);
	}

	/* No free block was found: try to flush the LRU list */

	buf_flush_free_margin();
	++srv_buf_pool_wait_free;

	os_aio_simulated_wake_handler_threads();

	buf_pool_mutex_enter();

	if (buf_pool->LRU_flush_ended > 0) {
		/* We have written pages in an LRU flush. To make the insert
		buffer more efficient, we try to move these pages to the free
		list. */

		buf_pool_mutex_exit();

		buf_LRU_try_free_flushed_blocks();
	} else {
		buf_pool_mutex_exit();
	}

	if (n_iterations > 10) {

		os_thread_sleep(500000);
	}

	n_iterations++;

	goto loop;
}

/***********************************************************************
Moves the LRU_old pointer so that the length of the old blocks list
is inside the allowed limits. */
UNIV_INLINE
void
buf_LRU_old_adjust_len(void)
/*========================*/
{
	ulint	old_len;
	ulint	new_len;

	ut_a(buf_pool->LRU_old);
	ut_ad(buf_pool_mutex_own());
#if 3 * (BUF_LRU_OLD_MIN_LEN / 8) <= BUF_LRU_OLD_TOLERANCE + 5
# error "3 * (BUF_LRU_OLD_MIN_LEN / 8) <= BUF_LRU_OLD_TOLERANCE + 5"
#endif

	for (;;) {
		old_len = buf_pool->LRU_old_len;
		new_len = 3 * (UT_LIST_GET_LEN(buf_pool->LRU) / 8);

		ut_ad(buf_pool->LRU_old->in_LRU_list);

		/* Update the LRU_old pointer if necessary */

		if (old_len < new_len - BUF_LRU_OLD_TOLERANCE) {

			buf_pool->LRU_old = UT_LIST_GET_PREV(
				LRU, buf_pool->LRU_old);
			buf_page_set_old(buf_pool->LRU_old, TRUE);
			buf_pool->LRU_old_len++;

		} else if (old_len > new_len + BUF_LRU_OLD_TOLERANCE) {

			buf_page_set_old(buf_pool->LRU_old, FALSE);
			buf_pool->LRU_old = UT_LIST_GET_NEXT(
				LRU, buf_pool->LRU_old);
			buf_pool->LRU_old_len--;
		} else {
			ut_a(buf_pool->LRU_old); /* Check that we did not
						 fall out of the LRU list */
			return;
		}
	}
}

/***********************************************************************
Initializes the old blocks pointer in the LRU list. This function should be
called when the LRU list grows to BUF_LRU_OLD_MIN_LEN length. */
static
void
buf_LRU_old_init(void)
/*==================*/
{
	buf_page_t*	bpage;

	ut_ad(buf_pool_mutex_own());
	ut_a(UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN);

	/* We first initialize all blocks in the LRU list as old and then use
	the adjust function to move the LRU_old pointer to the right
	position */

	bpage = UT_LIST_GET_FIRST(buf_pool->LRU);

	while (bpage != NULL) {
		ut_ad(bpage->in_LRU_list);
		buf_page_set_old(bpage, TRUE);
		bpage = UT_LIST_GET_NEXT(LRU, bpage);
	}

	buf_pool->LRU_old = UT_LIST_GET_FIRST(buf_pool->LRU);
	buf_pool->LRU_old_len = UT_LIST_GET_LEN(buf_pool->LRU);

	buf_LRU_old_adjust_len();
}

/**********************************************************************
Removes a block from the LRU list. */
UNIV_INLINE
void
buf_LRU_remove_block(
/*=================*/
	buf_page_t*	bpage)	/* in: control block */
{
	ut_ad(buf_pool);
	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own());

	ut_a(buf_page_in_file(bpage));

	ut_ad(bpage->in_LRU_list);

	/* If the LRU_old pointer is defined and points to just this block,
	move it backward one step */

	if (UNIV_UNLIKELY(bpage == buf_pool->LRU_old)) {

		/* Below: the previous block is guaranteed to exist, because
		the LRU_old pointer is only allowed to differ by the
		tolerance value from strict 3/8 of the LRU list length. */

		buf_pool->LRU_old = UT_LIST_GET_PREV(LRU, bpage);
		ut_a(buf_pool->LRU_old);
		buf_page_set_old(buf_pool->LRU_old, TRUE);

		buf_pool->LRU_old_len++;
	}

	/* Remove the block from the LRU list */
	UT_LIST_REMOVE(LRU, buf_pool->LRU, bpage);
	ut_d(bpage->in_LRU_list = FALSE);

	/* If the LRU list is so short that LRU_old not defined, return */
	if (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN) {

		buf_pool->LRU_old = NULL;

		return;
	}

	ut_ad(buf_pool->LRU_old);

	/* Update the LRU_old_len field if necessary */
	if (buf_page_is_old(bpage)) {

		buf_pool->LRU_old_len--;
	}

	/* Adjust the length of the old block list if necessary */
	buf_LRU_old_adjust_len();
}

/**********************************************************************
Adds a block to the LRU list end. */
UNIV_INLINE
void
buf_LRU_add_block_to_end_low(
/*=========================*/
	buf_page_t*	bpage)	/* in: control block */
{
	buf_page_t*	last_bpage;

	ut_ad(buf_pool);
	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own());

	ut_a(buf_page_in_file(bpage));

	buf_page_set_old(bpage, TRUE);

	last_bpage = UT_LIST_GET_LAST(buf_pool->LRU);

	if (last_bpage) {
		bpage->LRU_position = last_bpage->LRU_position;
	} else {
		bpage->LRU_position = buf_pool_clock_tic();
	}

	ut_ad(!bpage->in_LRU_list);
	UT_LIST_ADD_LAST(LRU, buf_pool->LRU, bpage);
	ut_d(bpage->in_LRU_list = TRUE);

	if (UT_LIST_GET_LEN(buf_pool->LRU) >= BUF_LRU_OLD_MIN_LEN) {

		buf_pool->LRU_old_len++;
	}

	if (UT_LIST_GET_LEN(buf_pool->LRU) > BUF_LRU_OLD_MIN_LEN) {

		ut_ad(buf_pool->LRU_old);

		/* Adjust the length of the old block list if necessary */

		buf_LRU_old_adjust_len();

	} else if (UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN) {

		/* The LRU list is now long enough for LRU_old to become
		defined: init it */

		buf_LRU_old_init();
	}
}

/**********************************************************************
Adds a block to the LRU list. */
UNIV_INLINE
void
buf_LRU_add_block_low(
/*==================*/
	buf_page_t*	bpage,	/* in: control block */
	ibool		old)	/* in: TRUE if should be put to the old blocks
				in the LRU list, else put to the start; if the
				LRU list is very short, the block is added to
				the start, regardless of this parameter */
{
	ut_ad(buf_pool);
	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own());

	ut_a(buf_page_in_file(bpage));
	ut_ad(!bpage->in_LRU_list);

	buf_page_set_old(bpage, old);

	if (!old || (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN)) {

		UT_LIST_ADD_FIRST(LRU, buf_pool->LRU, bpage);

		bpage->LRU_position = buf_pool_clock_tic();
		bpage->freed_page_clock = buf_pool->freed_page_clock;
	} else {
		UT_LIST_INSERT_AFTER(LRU, buf_pool->LRU, buf_pool->LRU_old,
				     bpage);
		buf_pool->LRU_old_len++;

		/* We copy the LRU position field of the previous block
		to the new block */

		bpage->LRU_position = (buf_pool->LRU_old)->LRU_position;
	}

	ut_d(bpage->in_LRU_list = TRUE);

	if (UT_LIST_GET_LEN(buf_pool->LRU) > BUF_LRU_OLD_MIN_LEN) {

		ut_ad(buf_pool->LRU_old);

		/* Adjust the length of the old block list if necessary */

		buf_LRU_old_adjust_len();

	} else if (UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN) {

		/* The LRU list is now long enough for LRU_old to become
		defined: init it */

		buf_LRU_old_init();
	}
}

/**********************************************************************
Adds a block to the LRU list. */
UNIV_INTERN
void
buf_LRU_add_block(
/*==============*/
	buf_page_t*	bpage,	/* in: control block */
	ibool		old)	/* in: TRUE if should be put to the old
				blocks in the LRU list, else put to the start;
				if the LRU list is very short, the block is
				added to the start, regardless of this
				parameter */
{
	buf_LRU_add_block_low(bpage, old);
}

/**********************************************************************
Moves a block to the start of the LRU list. */
UNIV_INTERN
void
buf_LRU_make_block_young(
/*=====================*/
	buf_page_t*	bpage)	/* in: control block */
{
	buf_LRU_remove_block(bpage);
	buf_LRU_add_block_low(bpage, FALSE);
}

/**********************************************************************
Moves a block to the end of the LRU list. */
UNIV_INTERN
void
buf_LRU_make_block_old(
/*===================*/
	buf_page_t*	bpage)	/* in: control block */
{
	buf_LRU_remove_block(bpage);
	buf_LRU_add_block_to_end_low(bpage);
}

/**********************************************************************
Try to free a block. */
UNIV_INTERN
ibool
buf_LRU_free_block(
/*===============*/
				/* out: TRUE if freed.  If bpage is a
				descriptor of a compressed-only page,
				the descriptor object will be freed
				as well.  If this function returns FALSE,
				it will not temporarily release
				buf_pool_mutex. */
	buf_page_t*	bpage,	/* in: block to be freed */
	ibool		zip,	/* in: TRUE if should remove also the
				compressed page of an uncompressed page */
	ibool*		buf_pool_mutex_released)
				/* in: pointer to a variable that will
				be assigned TRUE if buf_pool_mutex
				was temporarily released, or NULL */
{
	buf_page_t*	b = NULL;
	mutex_t*	block_mutex = buf_page_get_mutex(bpage);

	ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(block_mutex));
	ut_ad(buf_page_in_file(bpage));
	ut_ad(bpage->in_LRU_list);
	ut_ad(!bpage->in_flush_list == !bpage->oldest_modification);
	UNIV_MEM_ASSERT_RW(bpage, sizeof *bpage);

	if (!buf_page_can_relocate(bpage)) {

		/* Do not free buffer-fixed or I/O-fixed blocks. */
		return(FALSE);
	}

	if (zip || !bpage->zip.data) {
		/* This would completely free the block. */
		/* Do not completely free dirty blocks. */

		if (bpage->oldest_modification) {
			return(FALSE);
		}
	} else if (bpage->oldest_modification) {
		/* Do not completely free dirty blocks. */

		if (buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
			ut_ad(buf_page_get_state(bpage)
			      == BUF_BLOCK_ZIP_DIRTY);
			return(FALSE);
		}

		goto alloc;
	} else if (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE) {
		/* Allocate the control block for the compressed page.
		If it cannot be allocated (without freeing a block
		from the LRU list), refuse to free bpage. */
alloc:
		buf_pool_mutex_exit_forbid();
		b = buf_buddy_alloc(sizeof *b, NULL);
		buf_pool_mutex_exit_allow();

		if (UNIV_UNLIKELY(!b)) {
			return(FALSE);
		}

		memcpy(b, bpage, sizeof *b);
	}

#ifdef UNIV_DEBUG
	if (buf_debug_prints) {
		fprintf(stderr, "Putting space %lu page %lu to free list\n",
			(ulong) buf_page_get_space(bpage),
			(ulong) buf_page_get_page_no(bpage));
	}
#endif /* UNIV_DEBUG */

	if (buf_LRU_block_remove_hashed_page(bpage, zip)
	    != BUF_BLOCK_ZIP_FREE) {
		ut_a(bpage->buf_fix_count == 0);

		if (b) {
			buf_page_t*	prev_b	= UT_LIST_GET_PREV(LRU, b);
			const ulint	fold	= buf_page_address_fold(
				bpage->space, bpage->offset);

			ut_a(!buf_page_hash_get(bpage->space, bpage->offset));

			b->state = b->oldest_modification
				? BUF_BLOCK_ZIP_DIRTY
				: BUF_BLOCK_ZIP_PAGE;
			UNIV_MEM_DESC(b->zip.data,
				      page_zip_get_size(&b->zip), b);

			/* The fields in_page_hash and in_LRU_list of
			the to-be-freed block descriptor should have
			been cleared in
			buf_LRU_block_remove_hashed_page(), which
			invokes buf_LRU_remove_block(). */
			ut_ad(!bpage->in_page_hash);
			ut_ad(!bpage->in_LRU_list);

			/* The fields of bpage were copied to b before
			buf_LRU_block_remove_hashed_page() was invoked. */
			ut_ad(!b->in_zip_hash);
			ut_ad(b->in_page_hash);
			ut_ad(b->in_LRU_list);

			HASH_INSERT(buf_page_t, hash,
				    buf_pool->page_hash, fold, b);

			/* Insert b where bpage was in the LRU list. */
			if (UNIV_LIKELY(prev_b != NULL)) {
				ulint	lru_len;

				ut_ad(prev_b->in_LRU_list);
				ut_ad(buf_page_in_file(prev_b));
				UNIV_MEM_ASSERT_RW(prev_b, sizeof *prev_b);

				UT_LIST_INSERT_AFTER(LRU, buf_pool->LRU,
						     prev_b, b);

				if (buf_page_is_old(b)) {
					buf_pool->LRU_old_len++;
				}

				lru_len = UT_LIST_GET_LEN(buf_pool->LRU);

				if (lru_len > BUF_LRU_OLD_MIN_LEN) {
					ut_ad(buf_pool->LRU_old);
					/* Adjust the length of the
					old block list if necessary */
					buf_LRU_old_adjust_len();
				} else if (lru_len == BUF_LRU_OLD_MIN_LEN) {
					/* The LRU list is now long
					enough for LRU_old to become
					defined: init it */
					buf_LRU_old_init();
				}
			} else {
				ut_d(b->in_LRU_list = FALSE);
				buf_LRU_add_block_low(b, buf_page_is_old(b));
			}

			if (b->state == BUF_BLOCK_ZIP_PAGE) {
				buf_LRU_insert_zip_clean(b);
			} else {
				buf_page_t* prev;

				ut_ad(b->in_flush_list);
				ut_d(bpage->in_flush_list = FALSE);

				prev = UT_LIST_GET_PREV(list, b);
				UT_LIST_REMOVE(list, buf_pool->flush_list, b);

				if (prev) {
					ut_ad(prev->in_flush_list);
					UT_LIST_INSERT_AFTER(
						list,
						buf_pool->flush_list,
						prev, b);
				} else {
					UT_LIST_ADD_FIRST(
						list,
						buf_pool->flush_list,
						b);
				}
			}

			bpage->zip.data = NULL;
			page_zip_set_size(&bpage->zip, 0);

			/* Prevent buf_page_get_gen() from
			decompressing the block while we release
			buf_pool_mutex and block_mutex. */
			b->buf_fix_count++;
			b->io_fix = BUF_IO_READ;
		}

		if (buf_pool_mutex_released) {
			*buf_pool_mutex_released = TRUE;
		}

		buf_pool_mutex_exit();
		mutex_exit(block_mutex);

		/* Remove possible adaptive hash index on the page.
		The page was declared uninitialized by
		buf_LRU_block_remove_hashed_page().  We need to flag
		the contents of the page valid (which it still is) in
		order to avoid bogus Valgrind warnings.*/

		UNIV_MEM_VALID(((buf_block_t*) bpage)->frame,
			       UNIV_PAGE_SIZE);
		btr_search_drop_page_hash_index((buf_block_t*) bpage);
		UNIV_MEM_INVALID(((buf_block_t*) bpage)->frame,
				 UNIV_PAGE_SIZE);

		if (b) {
			/* Compute and stamp the compressed page
			checksum while not holding any mutex.  The
			block is already half-freed
			(BUF_BLOCK_REMOVE_HASH) and removed from
			buf_pool->page_hash, thus inaccessible by any
			other thread. */

			mach_write_to_4(
				b->zip.data + FIL_PAGE_SPACE_OR_CHKSUM,
				UNIV_LIKELY(srv_use_checksums)
				? page_zip_calc_checksum(
					b->zip.data,
					page_zip_get_size(&b->zip))
				: BUF_NO_CHECKSUM_MAGIC);
		}

		buf_pool_mutex_enter();
		mutex_enter(block_mutex);

		if (b) {
			mutex_enter(&buf_pool_zip_mutex);
			b->buf_fix_count--;
			buf_page_set_io_fix(b, BUF_IO_NONE);
			mutex_exit(&buf_pool_zip_mutex);
		}

		buf_LRU_block_free_hashed_page((buf_block_t*) bpage);
	} else {
		mutex_enter(block_mutex);
	}

	return(TRUE);
}

/**********************************************************************
Puts a block back to the free list. */
UNIV_INTERN
void
buf_LRU_block_free_non_file_page(
/*=============================*/
	buf_block_t*	block)	/* in: block, must not contain a file page */
{
	void*	data;

	ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(&block->mutex));
	ut_ad(block);

	switch (buf_block_get_state(block)) {
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_READY_FOR_USE:
		break;
	default:
		ut_error;
	}

	ut_ad(block->n_pointers == 0);
	ut_ad(!block->page.in_free_list);
	ut_ad(!block->page.in_flush_list);
	ut_ad(!block->page.in_LRU_list);

	buf_block_set_state(block, BUF_BLOCK_NOT_USED);

	UNIV_MEM_ALLOC(block->frame, UNIV_PAGE_SIZE);
#ifdef UNIV_DEBUG
	/* Wipe contents of page to reveal possible stale pointers to it */
	memset(block->frame, '\0', UNIV_PAGE_SIZE);
#else
	/* Wipe page_no and space_id */
	memset(block->frame + FIL_PAGE_OFFSET, 0xfe, 4);
	memset(block->frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xfe, 4);
#endif
	data = block->page.zip.data;

	if (data) {
		block->page.zip.data = NULL;
		mutex_exit(&block->mutex);
		buf_pool_mutex_exit_forbid();
		buf_buddy_free(data, page_zip_get_size(&block->page.zip));
		buf_pool_mutex_exit_allow();
		mutex_enter(&block->mutex);
		page_zip_set_size(&block->page.zip, 0);
	}

	UT_LIST_ADD_FIRST(list, buf_pool->free, (&block->page));
	ut_d(block->page.in_free_list = TRUE);

	UNIV_MEM_ASSERT_AND_FREE(block->frame, UNIV_PAGE_SIZE);
}

/**********************************************************************
Takes a block out of the LRU list and page hash table.
If the block is compressed-only (BUF_BLOCK_ZIP_PAGE),
the object will be freed and buf_pool_zip_mutex will be released.

If a compressed page or a compressed-only block descriptor is freed,
other compressed pages or compressed-only block descriptors may be
relocated. */
static
enum buf_page_state
buf_LRU_block_remove_hashed_page(
/*=============================*/
				/* out: the new state of the block
				(BUF_BLOCK_ZIP_FREE if the state was
				BUF_BLOCK_ZIP_PAGE, or BUF_BLOCK_REMOVE_HASH
				otherwise) */
	buf_page_t*	bpage,	/* in: block, must contain a file page and
				be in a state where it can be freed; there
				may or may not be a hash index to the page */
	ibool		zip)	/* in: TRUE if should remove also the
				compressed page of an uncompressed page */
{
	const buf_page_t*	hashed_bpage;
	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));

	ut_a(buf_page_get_io_fix(bpage) == BUF_IO_NONE);
	ut_a(bpage->buf_fix_count == 0);

	UNIV_MEM_ASSERT_RW(bpage, sizeof *bpage);

	buf_LRU_remove_block(bpage);

	buf_pool->freed_page_clock += 1;

	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_FILE_PAGE:
		UNIV_MEM_ASSERT_W(bpage, sizeof(buf_block_t));
		UNIV_MEM_ASSERT_W(((buf_block_t*) bpage)->frame,
				  UNIV_PAGE_SIZE);
		buf_block_modify_clock_inc((buf_block_t*) bpage);
		if (bpage->zip.data) {
			const page_t*	page = ((buf_block_t*) bpage)->frame;

			ut_a(!zip || bpage->oldest_modification == 0);

			switch (UNIV_EXPECT(fil_page_get_type(page),
					    FIL_PAGE_INDEX)) {
			case FIL_PAGE_TYPE_ALLOCATED:
			case FIL_PAGE_INODE:
			case FIL_PAGE_IBUF_BITMAP:
			case FIL_PAGE_TYPE_FSP_HDR:
			case FIL_PAGE_TYPE_XDES:
				/* These are essentially uncompressed pages. */
				if (!zip) {
					/* InnoDB writes the data to the
					uncompressed page frame.  Copy it
					to the compressed page, which will
					be preserved. */
					memcpy(bpage->zip.data, page,
					       page_zip_get_size(&bpage->zip));
				}
				break;
			case FIL_PAGE_TYPE_ZBLOB:
			case FIL_PAGE_TYPE_ZBLOB2:
				break;
			case FIL_PAGE_INDEX:
#ifdef UNIV_ZIP_DEBUG
				ut_a(page_zip_validate(&bpage->zip, page));
#endif /* UNIV_ZIP_DEBUG */
				break;
			default:
				ut_error;
			}

			break;
		}
		/* fall through */
	case BUF_BLOCK_ZIP_PAGE:
		ut_a(bpage->oldest_modification == 0);
		UNIV_MEM_ASSERT_W(bpage->zip.data,
				  page_zip_get_size(&bpage->zip));
		break;
	case BUF_BLOCK_ZIP_FREE:
	case BUF_BLOCK_ZIP_DIRTY:
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
		break;
	}

	hashed_bpage = buf_page_hash_get(bpage->space, bpage->offset);

	if (UNIV_UNLIKELY(bpage != hashed_bpage)) {
		fprintf(stderr,
			"InnoDB: Error: page %lu %lu not found"
			" in the hash table\n",
			(ulong) bpage->space,
			(ulong) bpage->offset);
		if (hashed_bpage) {
			fprintf(stderr,
				"InnoDB: In hash table we find block"
				" %p of %lu %lu which is not %p\n",
				(const void*) hashed_bpage,
				(ulong) hashed_bpage->space,
				(ulong) hashed_bpage->offset,
				(const void*) bpage);
		}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		mutex_exit(buf_page_get_mutex(bpage));
		buf_pool_mutex_exit();
		buf_print();
		buf_LRU_print();
		buf_validate();
		buf_LRU_validate();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		ut_error;
	}

	ut_ad(!bpage->in_zip_hash);
	ut_ad(bpage->in_page_hash);
	ut_d(bpage->in_page_hash = FALSE);
	HASH_DELETE(buf_page_t, hash, buf_pool->page_hash,
		    buf_page_address_fold(bpage->space, bpage->offset),
		    bpage);
	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_ZIP_PAGE:
		ut_ad(!bpage->in_free_list);
		ut_ad(!bpage->in_flush_list);
		ut_ad(!bpage->in_LRU_list);
		ut_a(bpage->zip.data);
		ut_a(buf_page_get_zip_size(bpage));

		UT_LIST_REMOVE(list, buf_pool->zip_clean, bpage);

		mutex_exit(&buf_pool_zip_mutex);
		buf_pool_mutex_exit_forbid();
		buf_buddy_free(bpage->zip.data,
			       page_zip_get_size(&bpage->zip));
		buf_buddy_free(bpage, sizeof(*bpage));
		buf_pool_mutex_exit_allow();
		UNIV_MEM_UNDESC(bpage);
		return(BUF_BLOCK_ZIP_FREE);

	case BUF_BLOCK_FILE_PAGE:
		memset(((buf_block_t*) bpage)->frame
		       + FIL_PAGE_OFFSET, 0xff, 4);
		memset(((buf_block_t*) bpage)->frame
		       + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xff, 4);
		UNIV_MEM_INVALID(((buf_block_t*) bpage)->frame,
				 UNIV_PAGE_SIZE);
		buf_page_set_state(bpage, BUF_BLOCK_REMOVE_HASH);

		if (zip && bpage->zip.data) {
			/* Free the compressed page. */
			void*	data = bpage->zip.data;
			bpage->zip.data = NULL;

			mutex_exit(&((buf_block_t*) bpage)->mutex);
			buf_buddy_free(data, page_zip_get_size(&bpage->zip));
			mutex_enter(&((buf_block_t*) bpage)->mutex);
			page_zip_set_size(&bpage->zip, 0);
		}

		return(BUF_BLOCK_REMOVE_HASH);

	case BUF_BLOCK_ZIP_FREE:
	case BUF_BLOCK_ZIP_DIRTY:
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		break;
	}

	ut_error;
	return(BUF_BLOCK_ZIP_FREE);
}

/**********************************************************************
Puts a file page whose has no hash index to the free list. */
static
void
buf_LRU_block_free_hashed_page(
/*===========================*/
	buf_block_t*	block)	/* in: block, must contain a file page and
				be in a state where it can be freed */
{
	ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(&block->mutex));

	buf_block_set_state(block, BUF_BLOCK_MEMORY);

	buf_LRU_block_free_non_file_page(block);
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/**************************************************************************
Validates the LRU list. */
UNIV_INTERN
ibool
buf_LRU_validate(void)
/*==================*/
{
	buf_page_t*	bpage;
	ulint		old_len;
	ulint		new_len;
	ulint		LRU_pos;

	ut_ad(buf_pool);
	buf_pool_mutex_enter();

	if (UT_LIST_GET_LEN(buf_pool->LRU) >= BUF_LRU_OLD_MIN_LEN) {

		ut_a(buf_pool->LRU_old);
		old_len = buf_pool->LRU_old_len;
		new_len = 3 * (UT_LIST_GET_LEN(buf_pool->LRU) / 8);
		ut_a(old_len >= new_len - BUF_LRU_OLD_TOLERANCE);
		ut_a(old_len <= new_len + BUF_LRU_OLD_TOLERANCE);
	}

	UT_LIST_VALIDATE(LRU, buf_page_t, buf_pool->LRU);

	bpage = UT_LIST_GET_FIRST(buf_pool->LRU);

	old_len = 0;

	while (bpage != NULL) {

		ut_a(buf_page_in_file(bpage));

		if (buf_page_is_old(bpage)) {
			old_len++;
		}

		if (buf_pool->LRU_old && (old_len == 1)) {
			ut_a(buf_pool->LRU_old == bpage);
		}

		LRU_pos	= buf_page_get_LRU_position(bpage);

		bpage = UT_LIST_GET_NEXT(LRU, bpage);

		if (bpage) {
			/* If the following assert fails, it may
			not be an error: just the buf_pool clock
			has wrapped around */
			ut_a(LRU_pos >= buf_page_get_LRU_position(bpage));
		}
	}

	if (buf_pool->LRU_old) {
		ut_a(buf_pool->LRU_old_len == old_len);
	}

	UT_LIST_VALIDATE(list, buf_page_t, buf_pool->free);

	for (bpage = UT_LIST_GET_FIRST(buf_pool->free);
	     bpage != NULL;
	     bpage = UT_LIST_GET_NEXT(list, bpage)) {

		ut_a(buf_page_get_state(bpage) == BUF_BLOCK_NOT_USED);
	}

	buf_pool_mutex_exit();
	return(TRUE);
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/**************************************************************************
Prints the LRU list. */
UNIV_INTERN
void
buf_LRU_print(void)
/*===============*/
{
	const buf_page_t*	bpage;

	ut_ad(buf_pool);
	buf_pool_mutex_enter();

	fprintf(stderr, "Pool ulint clock %lu\n",
		(ulong) buf_pool->ulint_clock);

	bpage = UT_LIST_GET_FIRST(buf_pool->LRU);

	while (bpage != NULL) {

		fprintf(stderr, "BLOCK space %lu page %lu ",
			(ulong) buf_page_get_space(bpage),
			(ulong) buf_page_get_page_no(bpage));

		if (buf_page_is_old(bpage)) {
			fputs("old ", stderr);
		}

		if (bpage->buf_fix_count) {
			fprintf(stderr, "buffix count %lu ",
				(ulong) bpage->buf_fix_count);
		}

		if (buf_page_get_io_fix(bpage)) {
			fprintf(stderr, "io_fix %lu ",
				(ulong) buf_page_get_io_fix(bpage));
		}

		if (bpage->oldest_modification) {
			fputs("modif. ", stderr);
		}

		switch (buf_page_get_state(bpage)) {
			const byte*	frame;
		case BUF_BLOCK_FILE_PAGE:
			frame = buf_block_get_frame((buf_block_t*) bpage);
			fprintf(stderr, "\nLRU pos %lu type %lu"
				" index id %lu\n",
				(ulong) buf_page_get_LRU_position(bpage),
				(ulong) fil_page_get_type(frame),
				(ulong) ut_dulint_get_low(
					btr_page_get_index_id(frame)));
			break;
		case BUF_BLOCK_ZIP_PAGE:
			frame = bpage->zip.data;
			fprintf(stderr, "\nLRU pos %lu type %lu size %lu"
				" index id %lu\n",
				(ulong) buf_page_get_LRU_position(bpage),
				(ulong) fil_page_get_type(frame),
				(ulong) buf_page_get_zip_size(bpage),
				(ulong) ut_dulint_get_low(
					btr_page_get_index_id(frame)));
			break;

		default:
			fprintf(stderr, "\nLRU pos %lu !state %lu!\n",
				(ulong) buf_page_get_LRU_position(bpage),
				(ulong) buf_page_get_state(bpage));
			break;
		}

		bpage = UT_LIST_GET_NEXT(LRU, bpage);
	}

	buf_pool_mutex_exit();
}
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG || UNIV_BUF_DEBUG */
