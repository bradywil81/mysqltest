/*   Innobase relational database engine; Copyright (C) 2001 Innobase Oy

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License 2
     as published by the Free Software Foundation in June 1991.

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
     GNU General Public License for more details.

     You should have received a copy of the GNU General Public License 2
     along with this program (in file COPYING); if not, write to the Free
     Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */
/******************************************************
The database buffer buf_pool

(c) 1995 Innobase Oy

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#include "buf0buf.h"

#ifdef UNIV_NONINL
#include "buf0buf.ic"
#endif

#include "buf0buddy.h"
#include "mem0mem.h"
#include "btr0btr.h"
#include "fil0fil.h"
#include "lock0lock.h"
#include "btr0sea.h"
#include "ibuf0ibuf.h"
#include "dict0dict.h"
#include "log0recv.h"
#include "log0log.h"
#include "trx0undo.h"
#include "srv0srv.h"
#include "page0zip.h"

/*
		IMPLEMENTATION OF THE BUFFER POOL
		=================================

Performance improvement:
------------------------
Thread scheduling in NT may be so slow that the OS wait mechanism should
not be used even in waiting for disk reads to complete.
Rather, we should put waiting query threads to the queue of
waiting jobs, and let the OS thread do something useful while the i/o
is processed. In this way we could remove most OS thread switches in
an i/o-intensive benchmark like TPC-C.

A possibility is to put a user space thread library between the database
and NT. User space thread libraries might be very fast.

SQL Server 7.0 can be configured to use 'fibers' which are lightweight
threads in NT. These should be studied.

		Buffer frames and blocks
		------------------------
Following the terminology of Gray and Reuter, we call the memory
blocks where file pages are loaded buffer frames. For each buffer
frame there is a control block, or shortly, a block, in the buffer
control array. The control info which does not need to be stored
in the file along with the file page, resides in the control block.

		Buffer pool struct
		------------------
The buffer buf_pool contains a single mutex which protects all the
control data structures of the buf_pool. The content of a buffer frame is
protected by a separate read-write lock in its control block, though.
These locks can be locked and unlocked without owning the buf_pool mutex.
The OS events in the buf_pool struct can be waited for without owning the
buf_pool mutex.

The buf_pool mutex is a hot-spot in main memory, causing a lot of
memory bus traffic on multiprocessor systems when processors
alternately access the mutex. On our Pentium, the mutex is accessed
maybe every 10 microseconds. We gave up the solution to have mutexes
for each control block, for instance, because it seemed to be
complicated.

A solution to reduce mutex contention of the buf_pool mutex is to
create a separate mutex for the page hash table. On Pentium,
accessing the hash table takes 2 microseconds, about half
of the total buf_pool mutex hold time.

		Control blocks
		--------------

The control block contains, for instance, the bufferfix count
which is incremented when a thread wants a file page to be fixed
in a buffer frame. The bufferfix operation does not lock the
contents of the frame, however. For this purpose, the control
block contains a read-write lock.

The buffer frames have to be aligned so that the start memory
address of a frame is divisible by the universal page size, which
is a power of two.

We intend to make the buffer buf_pool size on-line reconfigurable,
that is, the buf_pool size can be changed without closing the database.
Then the database administarator may adjust it to be bigger
at night, for example. The control block array must
contain enough control blocks for the maximum buffer buf_pool size
which is used in the particular database.
If the buf_pool size is cut, we exploit the virtual memory mechanism of
the OS, and just refrain from using frames at high addresses. Then the OS
can swap them to disk.

The control blocks containing file pages are put to a hash table
according to the file address of the page.
We could speed up the access to an individual page by using
"pointer swizzling": we could replace the page references on
non-leaf index pages by direct pointers to the page, if it exists
in the buf_pool. We could make a separate hash table where we could
chain all the page references in non-leaf pages residing in the buf_pool,
using the page reference as the hash key,
and at the time of reading of a page update the pointers accordingly.
Drawbacks of this solution are added complexity and,
possibly, extra space required on non-leaf pages for memory pointers.
A simpler solution is just to speed up the hash table mechanism
in the database, using tables whose size is a power of 2.

		Lists of blocks
		---------------

There are several lists of control blocks.

The free list (buf_pool->free) contains blocks which are currently not
used.

The LRU-list contains all the blocks holding a file page
except those for which the bufferfix count is non-zero.
The pages are in the LRU list roughly in the order of the last
access to the page, so that the oldest pages are at the end of the
list. We also keep a pointer to near the end of the LRU list,
which we can use when we want to artificially age a page in the
buf_pool. This is used if we know that some page is not needed
again for some time: we insert the block right after the pointer,
causing it to be replaced sooner than would noramlly be the case.
Currently this aging mechanism is used for read-ahead mechanism
of pages, and it can also be used when there is a scan of a full
table which cannot fit in the memory. Putting the pages near the
of the LRU list, we make sure that most of the buf_pool stays in the
main memory, undisturbed.

The chain of modified blocks (buf_pool->flush_list) contains the blocks
holding file pages that have been modified in the memory
but not written to disk yet. The block with the oldest modification
which has not yet been written to disk is at the end of the chain.

The chain of unmodified compressed blocks (buf_pool->zip_clean)
contains the control blocks (buf_page_t) of those compressed pages
that are not in buf_pool->flush_list and for which no uncompressed
page has been allocated in the buffer pool.  The control blocks for
uncompressed pages are accessible via buf_block_t objects that are
reachable via buf_pool->chunks[].

The chains of free memory blocks (buf_pool->zip_free[]) are used by
the buddy allocator (buf0buddy.c) to keep track of currently unused
memory blocks of size sizeof(buf_page_t)..UNIV_PAGE_SIZE / 2.  These
blocks are inside the UNIV_PAGE_SIZE-sized memory blocks of type
BUF_BLOCK_MEMORY that the buddy allocator requests from the buffer
pool.  The buddy allocator is solely used for allocating control
blocks for compressed pages (buf_page_t) and compressed page frames.

		Loading a file page
		-------------------

First, a victim block for replacement has to be found in the
buf_pool. It is taken from the free list or searched for from the
end of the LRU-list. An exclusive lock is reserved for the frame,
the io_fix field is set in the block fixing the block in buf_pool,
and the io-operation for loading the page is queued. The io-handler thread
releases the X-lock on the frame and resets the io_fix field
when the io operation completes.

A thread may request the above operation using the function
buf_page_get(). It may then continue to request a lock on the frame.
The lock is granted when the io-handler releases the x-lock.

		Read-ahead
		----------

The read-ahead mechanism is intended to be intelligent and
isolated from the semantically higher levels of the database
index management. From the higher level we only need the
information if a file page has a natural successor or
predecessor page. On the leaf level of a B-tree index,
these are the next and previous pages in the natural
order of the pages.

Let us first explain the read-ahead mechanism when the leafs
of a B-tree are scanned in an ascending or descending order.
When a read page is the first time referenced in the buf_pool,
the buffer manager checks if it is at the border of a so-called
linear read-ahead area. The tablespace is divided into these
areas of size 64 blocks, for example. So if the page is at the
border of such an area, the read-ahead mechanism checks if
all the other blocks in the area have been accessed in an
ascending or descending order. If this is the case, the system
looks at the natural successor or predecessor of the page,
checks if that is at the border of another area, and in this case
issues read-requests for all the pages in that area. Maybe
we could relax the condition that all the pages in the area
have to be accessed: if data is deleted from a table, there may
appear holes of unused pages in the area.

A different read-ahead mechanism is used when there appears
to be a random access pattern to a file.
If a new page is referenced in the buf_pool, and several pages
of its random access area (for instance, 32 consecutive pages
in a tablespace) have recently been referenced, we may predict
that the whole area may be needed in the near future, and issue
the read requests for the whole area.
*/

/* Value in microseconds */
static const int WAIT_FOR_READ	= 5000;

/* The buffer buf_pool of the database */
UNIV_INTERN buf_pool_t*	buf_pool = NULL;

/* mutex protecting the buffer pool struct and control blocks, except the
read-write lock in them */
UNIV_INTERN mutex_t		buf_pool_mutex;
/* mutex protecting the control blocks of compressed-only pages
(of type buf_page_t, not buf_block_t) */
UNIV_INTERN mutex_t		buf_pool_zip_mutex;

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
static ulint	buf_dbg_counter	= 0; /* This is used to insert validation
					operations in excution in the
					debug version */
/** Flag to forbid the release of the buffer pool mutex.
Protected by buf_pool->mutex. */
UNIV_INTERN ulint		buf_pool_mutex_exit_forbidden = 0;
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#ifdef UNIV_DEBUG
/* If this is set TRUE, the program prints info whenever
read-ahead or flush occurs */
UNIV_INTERN ibool		buf_debug_prints = FALSE;
#endif /* UNIV_DEBUG */

/* A chunk of buffers.  The buffer pool is allocated in chunks. */
struct buf_chunk_struct{
	ulint		mem_size;	/* allocated size of the chunk */
	ulint		size;		/* size of frames[] and blocks[] */
	void*		mem;		/* pointer to the memory area which
					was allocated for the frames */
	buf_block_t*	blocks;		/* array of buffer control blocks */
};

/************************************************************************
Calculates a page checksum which is stored to the page when it is written
to a file. Note that we must be careful to calculate the same value on
32-bit and 64-bit architectures. */
UNIV_INTERN
ulint
buf_calc_page_new_checksum(
/*=======================*/
				/* out: checksum */
	const byte*	page)	/* in: buffer page */
{
	ulint checksum;

	/* Since the field FIL_PAGE_FILE_FLUSH_LSN, and in versions <= 4.1.x
	..._ARCH_LOG_NO, are written outside the buffer pool to the first
	pages of data files, we have to skip them in the page checksum
	calculation.
	We must also skip the field FIL_PAGE_SPACE_OR_CHKSUM where the
	checksum is stored, and also the last 8 bytes of page because
	there we store the old formula checksum. */

	checksum = ut_fold_binary(page + FIL_PAGE_OFFSET,
				  FIL_PAGE_FILE_FLUSH_LSN - FIL_PAGE_OFFSET)
		+ ut_fold_binary(page + FIL_PAGE_DATA,
				 UNIV_PAGE_SIZE - FIL_PAGE_DATA
				 - FIL_PAGE_END_LSN_OLD_CHKSUM);
	checksum = checksum & 0xFFFFFFFFUL;

	return(checksum);
}

/************************************************************************
In versions < 4.0.14 and < 4.1.1 there was a bug that the checksum only
looked at the first few bytes of the page. This calculates that old
checksum.
NOTE: we must first store the new formula checksum to
FIL_PAGE_SPACE_OR_CHKSUM before calculating and storing this old checksum
because this takes that field as an input! */
UNIV_INTERN
ulint
buf_calc_page_old_checksum(
/*=======================*/
				/* out: checksum */
	const byte*	page)	/* in: buffer page */
{
	ulint checksum;

	checksum = ut_fold_binary(page, FIL_PAGE_FILE_FLUSH_LSN);

	checksum = checksum & 0xFFFFFFFFUL;

	return(checksum);
}

/************************************************************************
Checks if a page is corrupt. */
UNIV_INTERN
ibool
buf_page_is_corrupted(
/*==================*/
					/* out: TRUE if corrupted */
	const byte*	read_buf,	/* in: a database page */
	ulint		zip_size)	/* in: size of compressed page;
					0 for uncompressed pages */
{
	ulint		checksum_field;
	ulint		old_checksum_field;
#ifndef UNIV_HOTBACKUP
	ib_uint64_t	current_lsn;
#endif
	if (UNIV_LIKELY(!zip_size)
	    && memcmp(read_buf + FIL_PAGE_LSN + 4,
		      read_buf + UNIV_PAGE_SIZE
		      - FIL_PAGE_END_LSN_OLD_CHKSUM + 4, 4)) {

		/* Stored log sequence numbers at the start and the end
		of page do not match */

		return(TRUE);
	}

#ifndef UNIV_HOTBACKUP
	if (recv_lsn_checks_on && log_peek_lsn(&current_lsn)) {
		if (current_lsn < mach_read_ull(read_buf + FIL_PAGE_LSN)) {
			ut_print_timestamp(stderr);

			fprintf(stderr,
				"  InnoDB: Error: page %lu log sequence number"
				" %llu\n"
				"InnoDB: is in the future! Current system "
				"log sequence number %llu.\n"
				"InnoDB: Your database may be corrupt or "
				"you may have copied the InnoDB\n"
				"InnoDB: tablespace but not the InnoDB "
				"log files. See\n"
				"InnoDB: http://dev.mysql.com/doc/refman/"
				"5.1/en/forcing-recovery.html\n"
				"InnoDB: for more information.\n",
				(ulong) mach_read_from_4(read_buf
							 + FIL_PAGE_OFFSET),
				mach_read_ull(read_buf + FIL_PAGE_LSN),
				current_lsn);
		}
	}
#endif

	/* If we use checksums validation, make additional check before
	returning TRUE to ensure that the checksum is not equal to
	BUF_NO_CHECKSUM_MAGIC which might be stored by InnoDB with checksums
	disabled. Otherwise, skip checksum calculation and return FALSE */

	if (UNIV_LIKELY(srv_use_checksums)) {
		checksum_field = mach_read_from_4(read_buf
						  + FIL_PAGE_SPACE_OR_CHKSUM);

		if (UNIV_UNLIKELY(zip_size)) {
			return(checksum_field != BUF_NO_CHECKSUM_MAGIC
			       && checksum_field
			       != page_zip_calc_checksum(read_buf, zip_size));
		}

		old_checksum_field = mach_read_from_4(
			read_buf + UNIV_PAGE_SIZE
			- FIL_PAGE_END_LSN_OLD_CHKSUM);

		/* There are 2 valid formulas for old_checksum_field:

		1. Very old versions of InnoDB only stored 8 byte lsn to the
		start and the end of the page.

		2. Newer InnoDB versions store the old formula checksum
		there. */

		if (old_checksum_field != mach_read_from_4(read_buf
							   + FIL_PAGE_LSN)
		    && old_checksum_field != BUF_NO_CHECKSUM_MAGIC
		    && old_checksum_field
		    != buf_calc_page_old_checksum(read_buf)) {

			return(TRUE);
		}

		/* InnoDB versions < 4.0.14 and < 4.1.1 stored the space id
		(always equal to 0), to FIL_PAGE_SPACE_SPACE_OR_CHKSUM */

		if (checksum_field != 0
		    && checksum_field != BUF_NO_CHECKSUM_MAGIC
		    && checksum_field
		    != buf_calc_page_new_checksum(read_buf)) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/************************************************************************
Prints a page to stderr. */
UNIV_INTERN
void
buf_page_print(
/*===========*/
	const byte*	read_buf,	/* in: a database page */
	ulint		zip_size)	/* in: compressed page size, or
				0 for uncompressed pages */
{
	dict_index_t*	index;
	ulint		checksum;
	ulint		old_checksum;
	ulint		size	= zip_size;

	if (!size) {
		size = UNIV_PAGE_SIZE;
	}

	ut_print_timestamp(stderr);
	fprintf(stderr, "  InnoDB: Page dump in ascii and hex (%lu bytes):\n",
		(ulong) size);
	ut_print_buf(stderr, read_buf, size);
	fputs("InnoDB: End of page dump\n", stderr);

	if (zip_size) {
		/* Print compressed page. */

		switch (fil_page_get_type(read_buf)) {
		case FIL_PAGE_TYPE_ZBLOB:
		case FIL_PAGE_TYPE_ZBLOB2:
			checksum = srv_use_checksums
				? page_zip_calc_checksum(read_buf, zip_size)
				: BUF_NO_CHECKSUM_MAGIC;
			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: Compressed BLOB page"
				" checksum %lu, stored %lu\n"
				"InnoDB: Page lsn %lu %lu\n"
				"InnoDB: Page number (if stored"
				" to page already) %lu,\n"
				"InnoDB: space id (if stored"
				" to page already) %lu\n",
				(ulong) checksum,
				(ulong) mach_read_from_4(
					read_buf + FIL_PAGE_SPACE_OR_CHKSUM),
				(ulong) mach_read_from_4(
					read_buf + FIL_PAGE_LSN),
				(ulong) mach_read_from_4(
					read_buf + (FIL_PAGE_LSN + 4)),
				(ulong) mach_read_from_4(
					read_buf + FIL_PAGE_OFFSET),
				(ulong) mach_read_from_4(
					read_buf
					+ FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID));
			return;
		default:
			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: unknown page type %lu,"
				" assuming FIL_PAGE_INDEX\n",
				fil_page_get_type(read_buf));
			/* fall through */
		case FIL_PAGE_INDEX:
			checksum = srv_use_checksums
				? page_zip_calc_checksum(read_buf, zip_size)
				: BUF_NO_CHECKSUM_MAGIC;

			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: Compressed page checksum %lu,"
				" stored %lu\n"
				"InnoDB: Page lsn %lu %lu\n"
				"InnoDB: Page number (if stored"
				" to page already) %lu,\n"
				"InnoDB: space id (if stored"
				" to page already) %lu\n",
				(ulong) checksum,
				(ulong) mach_read_from_4(
					read_buf + FIL_PAGE_SPACE_OR_CHKSUM),
				(ulong) mach_read_from_4(
					read_buf + FIL_PAGE_LSN),
				(ulong) mach_read_from_4(
					read_buf + (FIL_PAGE_LSN + 4)),
				(ulong) mach_read_from_4(
					read_buf + FIL_PAGE_OFFSET),
				(ulong) mach_read_from_4(
					read_buf
					+ FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID));
			return;
		case FIL_PAGE_TYPE_XDES:
			/* This is an uncompressed page. */
			break;
		}
	}

	checksum = srv_use_checksums
		? buf_calc_page_new_checksum(read_buf) : BUF_NO_CHECKSUM_MAGIC;
	old_checksum = srv_use_checksums
		? buf_calc_page_old_checksum(read_buf) : BUF_NO_CHECKSUM_MAGIC;

	ut_print_timestamp(stderr);
	fprintf(stderr,
		"  InnoDB: Page checksum %lu, prior-to-4.0.14-form"
		" checksum %lu\n"
		"InnoDB: stored checksum %lu, prior-to-4.0.14-form"
		" stored checksum %lu\n"
		"InnoDB: Page lsn %lu %lu, low 4 bytes of lsn"
		" at page end %lu\n"
		"InnoDB: Page number (if stored to page already) %lu,\n"
		"InnoDB: space id (if created with >= MySQL-4.1.1"
		" and stored already) %lu\n",
		(ulong) checksum, (ulong) old_checksum,
		(ulong) mach_read_from_4(read_buf + FIL_PAGE_SPACE_OR_CHKSUM),
		(ulong) mach_read_from_4(read_buf + UNIV_PAGE_SIZE
					 - FIL_PAGE_END_LSN_OLD_CHKSUM),
		(ulong) mach_read_from_4(read_buf + FIL_PAGE_LSN),
		(ulong) mach_read_from_4(read_buf + FIL_PAGE_LSN + 4),
		(ulong) mach_read_from_4(read_buf + UNIV_PAGE_SIZE
					 - FIL_PAGE_END_LSN_OLD_CHKSUM + 4),
		(ulong) mach_read_from_4(read_buf + FIL_PAGE_OFFSET),
		(ulong) mach_read_from_4(read_buf
					 + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID));

	if (mach_read_from_2(read_buf + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE)
	    == TRX_UNDO_INSERT) {
		fprintf(stderr,
			"InnoDB: Page may be an insert undo log page\n");
	} else if (mach_read_from_2(read_buf + TRX_UNDO_PAGE_HDR
				    + TRX_UNDO_PAGE_TYPE)
		   == TRX_UNDO_UPDATE) {
		fprintf(stderr,
			"InnoDB: Page may be an update undo log page\n");
	}

	switch (fil_page_get_type(read_buf)) {
	case FIL_PAGE_INDEX:
		fprintf(stderr,
			"InnoDB: Page may be an index page where"
			" index id is %lu %lu\n",
			(ulong) ut_dulint_get_high(
				btr_page_get_index_id(read_buf)),
			(ulong) ut_dulint_get_low(
				btr_page_get_index_id(read_buf)));

#ifdef UNIV_HOTBACKUP
		/* If the code is in ibbackup, dict_sys may be uninitialized,
		i.e., NULL */

		if (dict_sys == NULL) {
			break;
		}
#endif /* UNIV_HOTBACKUP */

		index = dict_index_find_on_id_low(
			btr_page_get_index_id(read_buf));
		if (index) {
			fputs("InnoDB: (", stderr);
			dict_index_name_print(stderr, NULL, index);
			fputs(")\n", stderr);
		}
		break;
	case FIL_PAGE_INODE:
		fputs("InnoDB: Page may be an 'inode' page\n", stderr);
		break;
	case FIL_PAGE_IBUF_FREE_LIST:
		fputs("InnoDB: Page may be an insert buffer free list page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_ALLOCATED:
		fputs("InnoDB: Page may be a freshly allocated page\n",
		      stderr);
		break;
	case FIL_PAGE_IBUF_BITMAP:
		fputs("InnoDB: Page may be an insert buffer bitmap page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_SYS:
		fputs("InnoDB: Page may be a system page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_TRX_SYS:
		fputs("InnoDB: Page may be a transaction system page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_FSP_HDR:
		fputs("InnoDB: Page may be a file space header page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_XDES:
		fputs("InnoDB: Page may be an extent descriptor page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_BLOB:
		fputs("InnoDB: Page may be a BLOB page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_ZBLOB:
	case FIL_PAGE_TYPE_ZBLOB2:
		fputs("InnoDB: Page may be a compressed BLOB page\n",
		      stderr);
		break;
	}
}

/************************************************************************
Initializes a buffer control block when the buf_pool is created. */
static
void
buf_block_init(
/*===========*/
	buf_block_t*	block,	/* in: pointer to control block */
	byte*		frame)	/* in: pointer to buffer frame */
{
	UNIV_MEM_DESC(frame, UNIV_PAGE_SIZE, block);

	block->frame = frame;

	block->page.state = BUF_BLOCK_NOT_USED;
	block->page.buf_fix_count = 0;
	block->page.io_fix = BUF_IO_NONE;

	block->modify_clock = 0;

#ifdef UNIV_DEBUG_FILE_ACCESSES
	block->page.file_page_was_freed = FALSE;
#endif /* UNIV_DEBUG_FILE_ACCESSES */

	block->check_index_page_at_flush = FALSE;
	block->index = NULL;

#ifdef UNIV_DEBUG
	block->page.in_page_hash = FALSE;
	block->page.in_zip_hash = FALSE;
	block->page.in_flush_list = FALSE;
	block->page.in_free_list = FALSE;
	block->page.in_LRU_list = FALSE;
	block->n_pointers = 0;
#endif /* UNIV_DEBUG */
	page_zip_des_init(&block->page.zip);

	mutex_create(&block->mutex, SYNC_BUF_BLOCK);

	rw_lock_create(&block->lock, SYNC_LEVEL_VARYING);
	ut_ad(rw_lock_validate(&(block->lock)));

#ifdef UNIV_SYNC_DEBUG
	rw_lock_create(&block->debug_latch, SYNC_NO_ORDER_CHECK);
#endif /* UNIV_SYNC_DEBUG */
}

/************************************************************************
Allocates a chunk of buffer frames. */
static
buf_chunk_t*
buf_chunk_init(
/*===========*/
					/* out: chunk, or NULL on failure */
	buf_chunk_t*	chunk,		/* out: chunk of buffers */
	ulint		mem_size)	/* in: requested size in bytes */
{
	buf_block_t*	block;
	byte*		frame;
	ulint		i;

	/* Round down to a multiple of page size,
	although it already should be. */
	mem_size = ut_2pow_round(mem_size, UNIV_PAGE_SIZE);
	/* Reserve space for the block descriptors. */
	mem_size += ut_2pow_round((mem_size / UNIV_PAGE_SIZE) * (sizeof *block)
				  + (UNIV_PAGE_SIZE - 1), UNIV_PAGE_SIZE);

	chunk->mem_size = mem_size;
	chunk->mem = os_mem_alloc_large(&chunk->mem_size);

	if (UNIV_UNLIKELY(chunk->mem == NULL)) {

		return(NULL);
	}

	/* Allocate the block descriptors from
	the start of the memory block. */
	chunk->blocks = chunk->mem;

	/* Align a pointer to the first frame.  Note that when
	os_large_page_size is smaller than UNIV_PAGE_SIZE,
	we may allocate one fewer block than requested.  When
	it is bigger, we may allocate more blocks than requested. */

	frame = ut_align(chunk->mem, UNIV_PAGE_SIZE);
	chunk->size = chunk->mem_size / UNIV_PAGE_SIZE
		- (frame != chunk->mem);

	/* Subtract the space needed for block descriptors. */
	{
		ulint	size = chunk->size;

		while (frame < (byte*) (chunk->blocks + size)) {
			frame += UNIV_PAGE_SIZE;
			size--;
		}

		chunk->size = size;
	}

	/* Init block structs and assign frames for them. Then we
	assign the frames to the first blocks (we already mapped the
	memory above). */

	block = chunk->blocks;

	for (i = chunk->size; i--; ) {

		buf_block_init(block, frame);

#ifdef HAVE_purify
		/* Wipe contents of frame to eliminate a Purify warning */
		memset(block->frame, '\0', UNIV_PAGE_SIZE);
#endif
		/* Add the block to the free list */
		UT_LIST_ADD_LAST(list, buf_pool->free, (&block->page));
		ut_d(block->page.in_free_list = TRUE);

		block++;
		frame += UNIV_PAGE_SIZE;
	}

	return(chunk);
}

#ifdef UNIV_DEBUG
/*************************************************************************
Finds a block in the given buffer chunk that points to a
given compressed page. */
static
buf_block_t*
buf_chunk_contains_zip(
/*===================*/
				/* out: buffer block pointing to
				the compressed page, or NULL */
	buf_chunk_t*	chunk,	/* in: chunk being checked */
	const void*	data)	/* in: pointer to compressed page */
{
	buf_block_t*	block;
	ulint		i;

	ut_ad(buf_pool);
	ut_ad(buf_pool_mutex_own());

	block = chunk->blocks;

	for (i = chunk->size; i--; block++) {
		if (block->page.zip.data == data) {

			return(block);
		}
	}

	return(NULL);
}

/*************************************************************************
Finds a block in the buffer pool that points to a
given compressed page. */
UNIV_INTERN
buf_block_t*
buf_pool_contains_zip(
/*==================*/
				/* out: buffer block pointing to
				the compressed page, or NULL */
	const void*	data)	/* in: pointer to compressed page */
{
	ulint		n;
	buf_chunk_t*	chunk = buf_pool->chunks;

	for (n = buf_pool->n_chunks; n--; chunk++) {
		buf_block_t* block = buf_chunk_contains_zip(chunk, data);

		if (block) {
			return(block);
		}
	}

	return(NULL);
}
#endif /* UNIV_DEBUG */

/*************************************************************************
Checks that all file pages in the buffer chunk are in a replaceable state. */
static
const buf_block_t*
buf_chunk_not_freed(
/*================*/
				/* out: address of a non-free block,
				or NULL if all freed */
	buf_chunk_t*	chunk)	/* in: chunk being checked */
{
	buf_block_t*	block;
	ulint		i;

	ut_ad(buf_pool);
	ut_ad(buf_pool_mutex_own());

	block = chunk->blocks;

	for (i = chunk->size; i--; block++) {
		mutex_enter(&block->mutex);

		if (buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE
		    && !buf_flush_ready_for_replace(&block->page)) {

			mutex_exit(&block->mutex);
			return(block);
		}

		mutex_exit(&block->mutex);
	}

	return(NULL);
}

/*************************************************************************
Checks that all blocks in the buffer chunk are in BUF_BLOCK_NOT_USED state. */
static
ibool
buf_chunk_all_free(
/*===============*/
					/* out: TRUE if all freed */
	const buf_chunk_t*	chunk)	/* in: chunk being checked */
{
	const buf_block_t*	block;
	ulint			i;

	ut_ad(buf_pool);
	ut_ad(buf_pool_mutex_own());

	block = chunk->blocks;

	for (i = chunk->size; i--; block++) {

		if (buf_block_get_state(block) != BUF_BLOCK_NOT_USED) {

			return(FALSE);
		}
	}

	return(TRUE);
}

/************************************************************************
Frees a chunk of buffer frames. */
static
void
buf_chunk_free(
/*===========*/
	buf_chunk_t*	chunk)		/* out: chunk of buffers */
{
	buf_block_t*		block;
	const buf_block_t*	block_end;

	ut_ad(buf_pool_mutex_own());

	block_end = chunk->blocks + chunk->size;

	for (block = chunk->blocks; block < block_end; block++) {
		ut_a(buf_block_get_state(block) == BUF_BLOCK_NOT_USED);
		ut_a(!block->page.zip.data);

		ut_ad(!block->page.in_LRU_list);
		ut_ad(!block->page.in_flush_list);
		/* Remove the block from the free list. */
		ut_ad(block->page.in_free_list);
		UT_LIST_REMOVE(list, buf_pool->free, (&block->page));

		/* Free the latches. */
		mutex_free(&block->mutex);
		rw_lock_free(&block->lock);
#ifdef UNIV_SYNC_DEBUG
		rw_lock_free(&block->debug_latch);
#endif /* UNIV_SYNC_DEBUG */
		UNIV_MEM_UNDESC(block);
	}

	os_mem_free_large(chunk->mem, chunk->mem_size);
}

/************************************************************************
Creates the buffer pool. */
UNIV_INTERN
buf_pool_t*
buf_pool_init(void)
/*===============*/
				/* out, own: buf_pool object, NULL if not
				enough memory or error */
{
	buf_chunk_t*	chunk;
	ulint		i;

	buf_pool = mem_zalloc(sizeof(buf_pool_t));

	/* 1. Initialize general fields
	------------------------------- */
	mutex_create(&buf_pool_mutex, SYNC_BUF_POOL);
	mutex_create(&buf_pool_zip_mutex, SYNC_BUF_BLOCK);

	buf_pool_mutex_enter();

	buf_pool->n_chunks = 1;
	buf_pool->chunks = chunk = mem_alloc(sizeof *chunk);

	UT_LIST_INIT(buf_pool->free);

	if (!buf_chunk_init(chunk, srv_buf_pool_size)) {
		mem_free(chunk);
		mem_free(buf_pool);
		buf_pool = NULL;
		return(NULL);
	}

	srv_buf_pool_old_size = srv_buf_pool_size;
	buf_pool->curr_size = chunk->size;
	srv_buf_pool_curr_size = buf_pool->curr_size * UNIV_PAGE_SIZE;

	buf_pool->page_hash = hash_create(2 * buf_pool->curr_size);
	buf_pool->zip_hash = hash_create(2 * buf_pool->curr_size);

	buf_pool->last_printout_time = time(NULL);

	/* 2. Initialize flushing fields
	-------------------------------- */

	for (i = BUF_FLUSH_LRU; i < BUF_FLUSH_N_TYPES; i++) {
		buf_pool->no_flush[i] = os_event_create(NULL);
	}

	buf_pool->ulint_clock = 1;

	/* 3. Initialize LRU fields
	--------------------------- */
	/* All fields are initialized by mem_zalloc(). */

	buf_pool_mutex_exit();

	btr_search_sys_create(buf_pool->curr_size
			      * UNIV_PAGE_SIZE / sizeof(void*) / 64);

	/* 4. Initialize the buddy allocator fields */
	/* All fields are initialized by mem_zalloc(). */

	return(buf_pool);
}

/************************************************************************
Frees the buffer pool at shutdown.  This must not be invoked before
freeing all mutexes. */
UNIV_INTERN
void
buf_pool_free(void)
/*===============*/
{
	buf_chunk_t*	chunk;
	buf_chunk_t*	chunks;

	chunks = buf_pool->chunks;
	chunk = chunks + buf_pool->n_chunks;

	while (--chunk >= chunks) {
		/* Bypass the checks of buf_chunk_free(), since they
		would fail at shutdown. */
		os_mem_free_large(chunk->mem, chunk->mem_size);
	}

	buf_pool->n_chunks = 0;
}

/************************************************************************
Relocate a buffer control block.  Relocates the block on the LRU list
and in buf_pool->page_hash.  Does not relocate bpage->list. */
UNIV_INTERN
void
buf_relocate(
/*=========*/
	buf_page_t*	bpage,	/* control block being relocated */
	buf_page_t*	dpage)	/* destination control block */
{
	buf_page_t*	b;
	ulint		fold;

	ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_a(buf_page_get_io_fix(bpage) == BUF_IO_NONE);
	ut_a(bpage->buf_fix_count == 0);
	ut_a(buf_page_in_file(bpage));
	ut_ad(bpage->in_LRU_list);
	ut_ad(!bpage->in_zip_hash);
	ut_ad(bpage->in_page_hash);
	ut_ad(bpage == buf_page_hash_get(bpage->space, bpage->offset));

	memcpy(dpage, bpage, sizeof *dpage);

	ut_d(bpage->in_LRU_list = FALSE);
	ut_d(bpage->in_page_hash = FALSE);

	/* relocate buf_pool->LRU */
	b = UT_LIST_GET_PREV(LRU, bpage);
	UT_LIST_REMOVE(LRU, buf_pool->LRU, bpage);

	if (b) {
		UT_LIST_INSERT_AFTER(LRU, buf_pool->LRU, b, dpage);
	} else {
		UT_LIST_ADD_FIRST(LRU, buf_pool->LRU, dpage);
	}

	if (UNIV_UNLIKELY(buf_pool->LRU_old == bpage)) {
		buf_pool->LRU_old = dpage;
	}

	ut_d(UT_LIST_VALIDATE(LRU, buf_page_t, buf_pool->LRU));

	/* relocate buf_pool->page_hash */
	fold = buf_page_address_fold(bpage->space, bpage->offset);

	HASH_DELETE(buf_page_t, hash, buf_pool->page_hash, fold, bpage);
	HASH_INSERT(buf_page_t, hash, buf_pool->page_hash, fold, dpage);

	UNIV_MEM_INVALID(bpage, sizeof *bpage);
}

/************************************************************************
Shrinks the buffer pool. */
static
void
buf_pool_shrink(
/*============*/
				/* out: TRUE if shrunk */
	ulint	chunk_size)	/* in: number of pages to remove */
{
	buf_chunk_t*	chunks;
	buf_chunk_t*	chunk;
	ulint		max_size;
	ulint		max_free_size;
	buf_chunk_t*	max_chunk;
	buf_chunk_t*	max_free_chunk;

	ut_ad(!buf_pool_mutex_own());

try_again:
	btr_search_disable(); /* Empty the adaptive hash index again */
	buf_pool_mutex_enter();

shrink_again:
	if (buf_pool->n_chunks <= 1) {

		/* Cannot shrink if there is only one chunk */
		goto func_done;
	}

	/* Search for the largest free chunk
	not larger than the size difference */
	chunks = buf_pool->chunks;
	chunk = chunks + buf_pool->n_chunks;
	max_size = max_free_size = 0;
	max_chunk = max_free_chunk = NULL;

	while (--chunk >= chunks) {
		if (chunk->size <= chunk_size
		    && chunk->size > max_free_size) {
			if (chunk->size > max_size) {
				max_size = chunk->size;
				max_chunk = chunk;
			}

			if (buf_chunk_all_free(chunk)) {
				max_free_size = chunk->size;
				max_free_chunk = chunk;
			}
		}
	}

	if (!max_free_size) {

		ulint		dirty	= 0;
		ulint		nonfree	= 0;
		buf_block_t*	block;
		buf_block_t*	bend;

		/* Cannot shrink: try again later
		(do not assign srv_buf_pool_old_size) */
		if (!max_chunk) {

			goto func_exit;
		}

		block = max_chunk->blocks;
		bend = block + max_chunk->size;

		/* Move the blocks of chunk to the end of the
		LRU list and try to flush them. */
		for (; block < bend; block++) {
			switch (buf_block_get_state(block)) {
			case BUF_BLOCK_NOT_USED:
				continue;
			case BUF_BLOCK_FILE_PAGE:
				break;
			default:
				nonfree++;
				continue;
			}

			mutex_enter(&block->mutex);
			/* The following calls will temporarily
			release block->mutex and buf_pool_mutex.
			Therefore, we have to always retry,
			even if !dirty && !nonfree. */

			if (!buf_flush_ready_for_replace(&block->page)) {

				buf_LRU_make_block_old(&block->page);
				dirty++;
			} else if (!buf_LRU_free_block(&block->page,
						       TRUE, NULL)) {
				nonfree++;
			}

			mutex_exit(&block->mutex);
		}

		buf_pool_mutex_exit();

		/* Request for a flush of the chunk if it helps.
		Do not flush if there are non-free blocks, since
		flushing will not make the chunk freeable. */
		if (nonfree) {
			/* Avoid busy-waiting. */
			os_thread_sleep(100000);
		} else if (dirty
			   && buf_flush_batch(BUF_FLUSH_LRU, dirty, 0)
			   == ULINT_UNDEFINED) {

			buf_flush_wait_batch_end(BUF_FLUSH_LRU);
		}

		goto try_again;
	}

	max_size = max_free_size;
	max_chunk = max_free_chunk;

	srv_buf_pool_old_size = srv_buf_pool_size;

	/* Rewrite buf_pool->chunks.  Copy everything but max_chunk. */
	chunks = mem_alloc((buf_pool->n_chunks - 1) * sizeof *chunks);
	memcpy(chunks, buf_pool->chunks,
	       (max_chunk - buf_pool->chunks) * sizeof *chunks);
	memcpy(chunks + (max_chunk - buf_pool->chunks),
	       max_chunk + 1,
	       buf_pool->chunks + buf_pool->n_chunks
	       - (max_chunk + 1));
	ut_a(buf_pool->curr_size > max_chunk->size);
	buf_pool->curr_size -= max_chunk->size;
	srv_buf_pool_curr_size = buf_pool->curr_size * UNIV_PAGE_SIZE;
	chunk_size -= max_chunk->size;
	buf_chunk_free(max_chunk);
	mem_free(buf_pool->chunks);
	buf_pool->chunks = chunks;
	buf_pool->n_chunks--;

	/* Allow a slack of one megabyte. */
	if (chunk_size > 1048576 / UNIV_PAGE_SIZE) {

		goto shrink_again;
	}

func_done:
	srv_buf_pool_old_size = srv_buf_pool_size;
func_exit:
	buf_pool_mutex_exit();
	btr_search_enable();
}

/************************************************************************
Rebuild buf_pool->page_hash. */
static
void
buf_pool_page_hash_rebuild(void)
/*============================*/
{
	ulint		i;
	ulint		n_chunks;
	buf_chunk_t*	chunk;
	hash_table_t*	page_hash;
	hash_table_t*	zip_hash;
	buf_page_t*	b;

	buf_pool_mutex_enter();

	/* Free, create, and populate the hash table. */
	hash_table_free(buf_pool->page_hash);
	buf_pool->page_hash = page_hash = hash_create(2 * buf_pool->curr_size);
	zip_hash = hash_create(2 * buf_pool->curr_size);

	HASH_MIGRATE(buf_pool->zip_hash, zip_hash, buf_page_t, hash,
		     BUF_POOL_ZIP_FOLD_BPAGE);

	hash_table_free(buf_pool->zip_hash);
	buf_pool->zip_hash = zip_hash;

	/* Insert the uncompressed file pages to buf_pool->page_hash. */

	chunk = buf_pool->chunks;
	n_chunks = buf_pool->n_chunks;

	for (i = 0; i < n_chunks; i++, chunk++) {
		ulint		j;
		buf_block_t*	block = chunk->blocks;

		for (j = 0; j < chunk->size; j++, block++) {
			if (buf_block_get_state(block)
			    == BUF_BLOCK_FILE_PAGE) {
				ut_ad(!block->page.in_zip_hash);
				ut_ad(block->page.in_page_hash);

				HASH_INSERT(buf_page_t, hash, page_hash,
					    buf_page_address_fold(
						    block->page.space,
						    block->page.offset),
					    &block->page);
			}
		}
	}

	/* Insert the compressed-only pages to buf_pool->page_hash.
	All such blocks are either in buf_pool->zip_clean or
	in buf_pool->flush_list. */

	for (b = UT_LIST_GET_FIRST(buf_pool->zip_clean); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_a(buf_page_get_state(b) == BUF_BLOCK_ZIP_PAGE);
		ut_ad(!b->in_flush_list);
		ut_ad(b->in_LRU_list);
		ut_ad(b->in_page_hash);
		ut_ad(!b->in_zip_hash);

		HASH_INSERT(buf_page_t, hash, page_hash,
			    buf_page_address_fold(b->space, b->offset), b);
	}

	for (b = UT_LIST_GET_FIRST(buf_pool->flush_list); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_ad(b->in_flush_list);
		ut_ad(b->in_LRU_list);
		ut_ad(b->in_page_hash);
		ut_ad(!b->in_zip_hash);

		switch (buf_page_get_state(b)) {
		case BUF_BLOCK_ZIP_DIRTY:
			HASH_INSERT(buf_page_t, hash, page_hash,
				    buf_page_address_fold(b->space,
							  b->offset), b);
			break;
		case BUF_BLOCK_FILE_PAGE:
			/* uncompressed page */
			break;
		case BUF_BLOCK_ZIP_FREE:
		case BUF_BLOCK_ZIP_PAGE:
		case BUF_BLOCK_NOT_USED:
		case BUF_BLOCK_READY_FOR_USE:
		case BUF_BLOCK_MEMORY:
		case BUF_BLOCK_REMOVE_HASH:
			ut_error;
			break;
		}
	}

	buf_pool_mutex_exit();
}

/************************************************************************
Resizes the buffer pool. */
UNIV_INTERN
void
buf_pool_resize(void)
/*=================*/
{
	buf_pool_mutex_enter();

	if (srv_buf_pool_old_size == srv_buf_pool_size) {

		buf_pool_mutex_exit();
		return;
	}

	if (srv_buf_pool_curr_size + 1048576 > srv_buf_pool_size) {

		buf_pool_mutex_exit();

		/* Disable adaptive hash indexes and empty the index
		in order to free up memory in the buffer pool chunks. */
		buf_pool_shrink((srv_buf_pool_curr_size - srv_buf_pool_size)
				/ UNIV_PAGE_SIZE);
	} else if (srv_buf_pool_curr_size + 1048576 < srv_buf_pool_size) {

		/* Enlarge the buffer pool by at least one megabyte */

		ulint		mem_size
			= srv_buf_pool_size - srv_buf_pool_curr_size;
		buf_chunk_t*	chunks;
		buf_chunk_t*	chunk;

		chunks = mem_alloc((buf_pool->n_chunks + 1) * sizeof *chunks);

		memcpy(chunks, buf_pool->chunks, buf_pool->n_chunks
		       * sizeof *chunks);

		chunk = &chunks[buf_pool->n_chunks];

		if (!buf_chunk_init(chunk, mem_size)) {
			mem_free(chunks);
		} else {
			buf_pool->curr_size += chunk->size;
			srv_buf_pool_curr_size = buf_pool->curr_size
				* UNIV_PAGE_SIZE;
			mem_free(buf_pool->chunks);
			buf_pool->chunks = chunks;
			buf_pool->n_chunks++;
		}

		srv_buf_pool_old_size = srv_buf_pool_size;
		buf_pool_mutex_exit();
	}

	buf_pool_page_hash_rebuild();
}

/************************************************************************
Moves to the block to the start of the LRU list if there is a danger
that the block would drift out of the buffer pool. */
UNIV_INLINE
void
buf_block_make_young(
/*=================*/
	buf_page_t*	bpage)	/* in: block to make younger */
{
	ut_ad(!buf_pool_mutex_own());

	/* Note that we read freed_page_clock's without holding any mutex:
	this is allowed since the result is used only in heuristics */

	if (buf_page_peek_if_too_old(bpage)) {

		buf_pool_mutex_enter();
		/* There has been freeing activity in the LRU list:
		best to move to the head of the LRU list */

		buf_LRU_make_block_young(bpage);
		buf_pool_mutex_exit();
	}
}

/************************************************************************
Moves a page to the start of the buffer pool LRU list. This high-level
function can be used to prevent an important page from from slipping out of
the buffer pool. */
UNIV_INTERN
void
buf_page_make_young(
/*================*/
	buf_page_t*	bpage)	/* in: buffer block of a file page */
{
	buf_pool_mutex_enter();

	ut_a(buf_page_in_file(bpage));

	buf_LRU_make_block_young(bpage);

	buf_pool_mutex_exit();
}

/************************************************************************
Resets the check_index_page_at_flush field of a page if found in the buffer
pool. */
UNIV_INTERN
void
buf_reset_check_index_page_at_flush(
/*================================*/
	ulint	space,	/* in: space id */
	ulint	offset)	/* in: page number */
{
	buf_block_t*	block;

	buf_pool_mutex_enter();

	block = (buf_block_t*) buf_page_hash_get(space, offset);

	if (block && buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE) {
		block->check_index_page_at_flush = FALSE;
	}

	buf_pool_mutex_exit();
}

/************************************************************************
Returns the current state of is_hashed of a page. FALSE if the page is
not in the pool. NOTE that this operation does not fix the page in the
pool if it is found there. */
UNIV_INTERN
ibool
buf_page_peek_if_search_hashed(
/*===========================*/
			/* out: TRUE if page hash index is built in search
			system */
	ulint	space,	/* in: space id */
	ulint	offset)	/* in: page number */
{
	buf_block_t*	block;
	ibool		is_hashed;

	buf_pool_mutex_enter();

	block = (buf_block_t*) buf_page_hash_get(space, offset);

	if (!block || buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE) {
		is_hashed = FALSE;
	} else {
		is_hashed = block->is_hashed;
	}

	buf_pool_mutex_exit();

	return(is_hashed);
}

#ifdef UNIV_DEBUG_FILE_ACCESSES
/************************************************************************
Sets file_page_was_freed TRUE if the page is found in the buffer pool.
This function should be called when we free a file page and want the
debug version to check that it is not accessed any more unless
reallocated. */
UNIV_INTERN
buf_page_t*
buf_page_set_file_page_was_freed(
/*=============================*/
			/* out: control block if found in page hash table,
			otherwise NULL */
	ulint	space,	/* in: space id */
	ulint	offset)	/* in: page number */
{
	buf_page_t*	bpage;

	buf_pool_mutex_enter();

	bpage = buf_page_hash_get(space, offset);

	if (bpage) {
		bpage->file_page_was_freed = TRUE;
	}

	buf_pool_mutex_exit();

	return(bpage);
}

/************************************************************************
Sets file_page_was_freed FALSE if the page is found in the buffer pool.
This function should be called when we free a file page and want the
debug version to check that it is not accessed any more unless
reallocated. */
UNIV_INTERN
buf_page_t*
buf_page_reset_file_page_was_freed(
/*===============================*/
			/* out: control block if found in page hash table,
			otherwise NULL */
	ulint	space,	/* in: space id */
	ulint	offset)	/* in: page number */
{
	buf_page_t*	bpage;

	buf_pool_mutex_enter();

	bpage = buf_page_hash_get(space, offset);

	if (bpage) {
		bpage->file_page_was_freed = FALSE;
	}

	buf_pool_mutex_exit();

	return(bpage);
}
#endif /* UNIV_DEBUG_FILE_ACCESSES */

/************************************************************************
Get read access to a compressed page (usually of type
FIL_PAGE_TYPE_ZBLOB or FIL_PAGE_TYPE_ZBLOB2).
The page must be released with buf_page_release_zip().
NOTE: the page is not protected by any latch.  Mutual exclusion has to
be implemented at a higher level.  In other words, all possible
accesses to a given page through this function must be protected by
the same set of mutexes or latches. */
UNIV_INTERN
buf_page_t*
buf_page_get_zip(
/*=============*/
				/* out: pointer to the block */
	ulint		space,	/* in: space id */
	ulint		zip_size,/* in: compressed page size */
	ulint		offset)	/* in: page number */
{
	buf_page_t*	bpage;
	mutex_t*	block_mutex;
	ibool		must_read;

#ifndef UNIV_LOG_DEBUG
	ut_ad(!ibuf_inside());
#endif
	buf_pool->n_page_gets++;

	for (;;) {
		buf_pool_mutex_enter();
lookup:
		bpage = buf_page_hash_get(space, offset);
		if (bpage) {
			break;
		}

		/* Page not in buf_pool: needs to be read from file */

		buf_pool_mutex_exit();

		buf_read_page(space, zip_size, offset);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		ut_a(++buf_dbg_counter % 37 || buf_validate());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
	}

	if (UNIV_UNLIKELY(!bpage->zip.data)) {
		/* There is no compressed page. */
		buf_pool_mutex_exit();
		return(NULL);
	}

	block_mutex = buf_page_get_mutex(bpage);
	mutex_enter(block_mutex);

	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
	case BUF_BLOCK_ZIP_FREE:
		ut_error;
		break;
	case BUF_BLOCK_ZIP_PAGE:
	case BUF_BLOCK_ZIP_DIRTY:
		bpage->buf_fix_count++;
		break;
	case BUF_BLOCK_FILE_PAGE:
		/* Discard the uncompressed page frame if possible. */
		if (buf_LRU_free_block(bpage, FALSE, NULL)) {

			mutex_exit(block_mutex);
			goto lookup;
		}

		buf_block_buf_fix_inc((buf_block_t*) bpage,
				      __FILE__, __LINE__);
		break;
	}

	must_read = buf_page_get_io_fix(bpage) == BUF_IO_READ;

	buf_pool_mutex_exit();

	buf_page_set_accessed(bpage, TRUE);

	mutex_exit(block_mutex);

	buf_block_make_young(bpage);

#ifdef UNIV_DEBUG_FILE_ACCESSES
	ut_a(!bpage->file_page_was_freed);
#endif

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
	ut_a(bpage->buf_fix_count > 0);
	ut_a(buf_page_in_file(bpage));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	if (must_read) {
		/* Let us wait until the read operation
		completes */

		for (;;) {
			enum buf_io_fix	io_fix;

			mutex_enter(block_mutex);
			io_fix = buf_page_get_io_fix(bpage);
			mutex_exit(block_mutex);

			if (io_fix == BUF_IO_READ) {

				os_thread_sleep(WAIT_FOR_READ);
			} else {
				break;
			}
		}
	}

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(buf_page_get_space(bpage),
			    buf_page_get_page_no(bpage)) == 0);
#endif
	return(bpage);
}

/************************************************************************
Initialize some fields of a control block. */
UNIV_INLINE
void
buf_block_init_low(
/*===============*/
	buf_block_t*	block)	/* in: block to init */
{
	block->check_index_page_at_flush = FALSE;
	block->index		= NULL;

	block->n_hash_helps	= 0;
	block->is_hashed	= FALSE;
	block->n_fields		= 1;
	block->n_bytes		= 0;
	block->left_side	= TRUE;
}

/************************************************************************
Decompress a block. */
static
ibool
buf_zip_decompress(
/*===============*/
				/* out: TRUE if successful */
	buf_block_t*	block,	/* in/out: block */
	ibool		check)	/* in: TRUE=verify the page checksum */
{
	const byte* frame = block->page.zip.data;

	ut_ad(buf_block_get_zip_size(block));
	ut_a(buf_block_get_space(block) != 0);

	if (UNIV_LIKELY(check)) {
		ulint	stamp_checksum	= mach_read_from_4(
			frame + FIL_PAGE_SPACE_OR_CHKSUM);
		ulint	calc_checksum	= page_zip_calc_checksum(
			frame, page_zip_get_size(&block->page.zip));

		if (UNIV_UNLIKELY(stamp_checksum != calc_checksum)) {
			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: compressed page checksum mismatch"
				" (space %u page %u): %lu != %lu\n",
				block->page.space, block->page.offset,
				stamp_checksum, calc_checksum);
			return(FALSE);
		}
	}

	switch (fil_page_get_type(frame)) {
	case FIL_PAGE_INDEX:
		if (page_zip_decompress(&block->page.zip,
					block->frame)) {
			return(TRUE);
		}

		fprintf(stderr,
			"InnoDB: unable to decompress space %lu page %lu\n",
			(ulong) block->page.space,
			(ulong) block->page.offset);
		return(FALSE);

	case FIL_PAGE_TYPE_ALLOCATED:
	case FIL_PAGE_INODE:
	case FIL_PAGE_IBUF_BITMAP:
	case FIL_PAGE_TYPE_FSP_HDR:
	case FIL_PAGE_TYPE_XDES:
	case FIL_PAGE_TYPE_ZBLOB:
	case FIL_PAGE_TYPE_ZBLOB2:
		/* Copy to uncompressed storage. */
		memcpy(block->frame, frame,
		       buf_block_get_zip_size(block));
		return(TRUE);
	}

	ut_print_timestamp(stderr);
	fprintf(stderr,
		"  InnoDB: unknown compressed page"
		" type %lu\n",
		fil_page_get_type(frame));
	return(FALSE);
}

/************************************************************************
Find out if a buffer block was created by buf_chunk_init(). */
static
ibool
buf_block_is_uncompressed(
/*======================*/
					/* out: TRUE if "block" has
					been added to buf_pool->free
					by buf_chunk_init() */
	const buf_block_t*	block)	/* in: pointer to block,
					not dereferenced */
{
	const buf_chunk_t*		chunk	= buf_pool->chunks;
	const buf_chunk_t* const	echunk	= chunk + buf_pool->n_chunks;

	ut_ad(buf_pool_mutex_own());

	if (UNIV_UNLIKELY((((ulint) block) % sizeof *block) != 0)) {
		/* The pointer should be aligned. */
		return(FALSE);
	}

	while (chunk < echunk) {
		if (block >= chunk->blocks
		    && block < chunk->blocks + chunk->size) {

			return(TRUE);
		}

		chunk++;
	}

	return(FALSE);
}

/************************************************************************
This is the general function used to get access to a database page. */
UNIV_INTERN
buf_block_t*
buf_page_get_gen(
/*=============*/
				/* out: pointer to the block or NULL */
	ulint		space,	/* in: space id */
	ulint		zip_size,/* in: compressed page size in bytes
				or 0 for uncompressed pages */
	ulint		offset,	/* in: page number */
	ulint		rw_latch,/* in: RW_S_LATCH, RW_X_LATCH, RW_NO_LATCH */
	buf_block_t*	guess,	/* in: guessed block or NULL */
	ulint		mode,	/* in: BUF_GET, BUF_GET_IF_IN_POOL,
				BUF_GET_NO_LATCH, BUF_GET_NOWAIT */
	const char*	file,	/* in: file name */
	ulint		line,	/* in: line where called */
	mtr_t*		mtr)	/* in: mini-transaction */
{
	buf_block_t*	block;
	ibool		accessed;
	ulint		fix_type;
	ibool		must_read;

	ut_ad(mtr);
	ut_ad((rw_latch == RW_S_LATCH)
	      || (rw_latch == RW_X_LATCH)
	      || (rw_latch == RW_NO_LATCH));
	ut_ad((mode != BUF_GET_NO_LATCH) || (rw_latch == RW_NO_LATCH));
	ut_ad((mode == BUF_GET) || (mode == BUF_GET_IF_IN_POOL)
	      || (mode == BUF_GET_NO_LATCH) || (mode == BUF_GET_NOWAIT));
	ut_ad(zip_size == fil_space_get_zip_size(space));
#ifndef UNIV_LOG_DEBUG
	ut_ad(!ibuf_inside() || ibuf_page(space, zip_size, offset));
#endif
	buf_pool->n_page_gets++;
loop:
	block = guess;
	buf_pool_mutex_enter();

	if (block) {
		/* If the guess is a compressed page descriptor that
		has been allocated by buf_buddy_alloc(), it may have
		been invalidated by buf_buddy_relocate().  In that
		case, block could point to something that happens to
		contain the expected bits in block->page.  Similarly,
		the guess may be pointing to a buffer pool chunk that
		has been released when resizing the buffer pool. */

		if (!buf_block_is_uncompressed(block)
		    || offset != block->page.offset
		    || space != block->page.space
		    || buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE) {

			block = guess = NULL;
		} else {
			ut_ad(!block->page.in_zip_hash);
			ut_ad(block->page.in_page_hash);
		}
	}

	if (block == NULL) {
		block = (buf_block_t*) buf_page_hash_get(space, offset);
	}

loop2:
	if (block == NULL) {
		/* Page not in buf_pool: needs to be read from file */

		buf_pool_mutex_exit();

		if (mode == BUF_GET_IF_IN_POOL) {

			return(NULL);
		}

		buf_read_page(space, zip_size, offset);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		ut_a(++buf_dbg_counter % 37 || buf_validate());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		goto loop;
	}

	ut_ad(page_zip_get_size(&block->page.zip) == zip_size);

	must_read = buf_block_get_io_fix(block) == BUF_IO_READ;

	if (must_read && mode == BUF_GET_IF_IN_POOL) {
		/* The page is only being read to buffer */
		buf_pool_mutex_exit();

		return(NULL);
	}

	switch (buf_block_get_state(block)) {
		buf_page_t*	bpage;
		ibool		success;

	case BUF_BLOCK_FILE_PAGE:
		break;

	case BUF_BLOCK_ZIP_PAGE:
	case BUF_BLOCK_ZIP_DIRTY:
		bpage = &block->page;

		if (bpage->buf_fix_count
		    || buf_page_get_io_fix(bpage) != BUF_IO_NONE) {
			/* This condition often occurs when the buffer
			is not buffer-fixed, but I/O-fixed by
			buf_page_init_for_read(). */
wait_until_unfixed:
			/* The block is buffer-fixed or I/O-fixed.
			Try again later. */
			buf_pool_mutex_exit();
			os_thread_sleep(WAIT_FOR_READ);

			goto loop;
		}

		/* Allocate an uncompressed page. */
		buf_pool_mutex_exit();

		block = buf_LRU_get_free_block(0);
		ut_a(block);

		buf_pool_mutex_enter();
		mutex_enter(&block->mutex);

		{
			buf_page_t*	hash_bpage
				= buf_page_hash_get(space, offset);

			if (UNIV_UNLIKELY(bpage != hash_bpage)) {
				/* The buf_pool->page_hash was modified
				while buf_pool_mutex was released.
				Free the block that was allocated. */

				buf_LRU_block_free_non_file_page(block);
				mutex_exit(&block->mutex);

				block = (buf_block_t*) hash_bpage;
				goto loop2;
			}
		}

		if (UNIV_UNLIKELY
		    (bpage->buf_fix_count
		     || buf_page_get_io_fix(bpage) != BUF_IO_NONE)) {

			/* The block was buffer-fixed or I/O-fixed
			while buf_pool_mutex was not held by this thread.
			Free the block that was allocated and try again.
			This should be extremely unlikely. */

			buf_LRU_block_free_non_file_page(block);
			mutex_exit(&block->mutex);

			goto wait_until_unfixed;
		}

		/* Move the compressed page from bpage to block,
		and uncompress it. */

		mutex_enter(&buf_pool_zip_mutex);

		buf_relocate(bpage, &block->page);
		buf_block_init_low(block);
		block->lock_hash_val = lock_rec_hash(space, offset);

		UNIV_MEM_DESC(&block->page.zip.data,
			      page_zip_get_size(&block->page.zip), block);

		if (buf_page_get_state(&block->page)
		    == BUF_BLOCK_ZIP_PAGE) {
			UT_LIST_REMOVE(list, buf_pool->zip_clean,
				       &block->page);
			ut_ad(!block->page.in_flush_list);
		} else {
			/* Relocate buf_pool->flush_list. */
			buf_page_t*	b;

			b = UT_LIST_GET_PREV(list, &block->page);
			ut_ad(block->page.in_flush_list);
			UT_LIST_REMOVE(list, buf_pool->flush_list,
				       &block->page);

			if (b) {
				UT_LIST_INSERT_AFTER(
					list, buf_pool->flush_list, b,
					&block->page);
			} else {
				UT_LIST_ADD_FIRST(
					list, buf_pool->flush_list,
					&block->page);
			}
		}

		/* Buffer-fix, I/O-fix, and X-latch the block
		for the duration of the decompression. */
		block->page.state = BUF_BLOCK_FILE_PAGE;
		block->page.buf_fix_count = 1;
		buf_block_set_io_fix(block, BUF_IO_READ);
		buf_pool->n_pend_unzip++;
		rw_lock_x_lock(&block->lock);
		mutex_exit(&block->mutex);
		mutex_exit(&buf_pool_zip_mutex);

		buf_buddy_free(bpage, sizeof *bpage);

		buf_pool_mutex_exit();

		/* Decompress the page and apply buffered operations
		while not holding buf_pool_mutex or block->mutex. */
		success = buf_zip_decompress(block, srv_use_checksums);

		if (UNIV_LIKELY(success)) {
			ibuf_merge_or_delete_for_page(block, space, offset,
						      zip_size, TRUE);
		}

		/* Unfix and unlatch the block. */
		buf_pool_mutex_enter();
		mutex_enter(&block->mutex);
		buf_pool->n_pend_unzip--;
		block->page.buf_fix_count--;
		buf_block_set_io_fix(block, BUF_IO_NONE);
		mutex_exit(&block->mutex);
		rw_lock_x_unlock(&block->lock);

		if (UNIV_UNLIKELY(!success)) {

			buf_pool_mutex_exit();
			return(NULL);
		}

		break;

	case BUF_BLOCK_ZIP_FREE:
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
		break;
	}

	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

	mutex_enter(&block->mutex);
	UNIV_MEM_ASSERT_RW(&block->page, sizeof block->page);

	buf_block_buf_fix_inc(block, file, line);
	buf_pool_mutex_exit();

	/* Check if this is the first access to the page */

	accessed = buf_page_is_accessed(&block->page);

	buf_page_set_accessed(&block->page, TRUE);

	mutex_exit(&block->mutex);

	buf_block_make_young(&block->page);

#ifdef UNIV_DEBUG_FILE_ACCESSES
	ut_a(!block->page.file_page_was_freed);
#endif

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
	ut_a(block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	if (mode == BUF_GET_NOWAIT) {
		ibool	success;

		if (rw_latch == RW_S_LATCH) {
			success = rw_lock_s_lock_func_nowait(&(block->lock),
							     file, line);
			fix_type = MTR_MEMO_PAGE_S_FIX;
		} else {
			ut_ad(rw_latch == RW_X_LATCH);
			success = rw_lock_x_lock_func_nowait(&(block->lock),
							     file, line);
			fix_type = MTR_MEMO_PAGE_X_FIX;
		}

		if (!success) {
			mutex_enter(&block->mutex);
			buf_block_buf_fix_dec(block);
			mutex_exit(&block->mutex);

			return(NULL);
		}
	} else if (rw_latch == RW_NO_LATCH) {

		if (must_read) {
			/* Let us wait until the read operation
			completes */

			for (;;) {
				enum buf_io_fix	io_fix;

				mutex_enter(&block->mutex);
				io_fix = buf_block_get_io_fix(block);
				mutex_exit(&block->mutex);

				if (io_fix == BUF_IO_READ) {

					os_thread_sleep(WAIT_FOR_READ);
				} else {
					break;
				}
			}
		}

		fix_type = MTR_MEMO_BUF_FIX;
	} else if (rw_latch == RW_S_LATCH) {

		rw_lock_s_lock_func(&(block->lock), 0, file, line);

		fix_type = MTR_MEMO_PAGE_S_FIX;
	} else {
		rw_lock_x_lock_func(&(block->lock), 0, file, line);

		fix_type = MTR_MEMO_PAGE_X_FIX;
	}

	mtr_memo_push(mtr, block, fix_type);

	if (!accessed) {
		/* In the case of a first access, try to apply linear
		read-ahead */

		buf_read_ahead_linear(space, zip_size, offset);
	}

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(buf_block_get_space(block),
			    buf_block_get_page_no(block)) == 0);
#endif
	return(block);
}

/************************************************************************
This is the general function used to get optimistic access to a database
page. */
UNIV_INTERN
ibool
buf_page_optimistic_get_func(
/*=========================*/
				/* out: TRUE if success */
	ulint		rw_latch,/* in: RW_S_LATCH, RW_X_LATCH */
	buf_block_t*	block,	/* in: guessed buffer block */
	ib_uint64_t	modify_clock,/* in: modify clock value if mode is
				..._GUESS_ON_CLOCK */
	const char*	file,	/* in: file name */
	ulint		line,	/* in: line where called */
	mtr_t*		mtr)	/* in: mini-transaction */
{
	ibool		accessed;
	ibool		success;
	ulint		fix_type;

	ut_ad(mtr && block);
	ut_ad((rw_latch == RW_S_LATCH) || (rw_latch == RW_X_LATCH));

	mutex_enter(&block->mutex);

	if (UNIV_UNLIKELY(buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE)) {

		mutex_exit(&block->mutex);

		return(FALSE);
	}

	buf_block_buf_fix_inc(block, file, line);
	accessed = buf_page_is_accessed(&block->page);
	buf_page_set_accessed(&block->page, TRUE);

	mutex_exit(&block->mutex);

	buf_block_make_young(&block->page);

	/* Check if this is the first access to the page */

	ut_ad(!ibuf_inside()
	      || ibuf_page(buf_block_get_space(block),
			   buf_block_get_zip_size(block),
			   buf_block_get_page_no(block)));

	if (rw_latch == RW_S_LATCH) {
		success = rw_lock_s_lock_func_nowait(&(block->lock),
						     file, line);
		fix_type = MTR_MEMO_PAGE_S_FIX;
	} else {
		success = rw_lock_x_lock_func_nowait(&(block->lock),
						     file, line);
		fix_type = MTR_MEMO_PAGE_X_FIX;
	}

	if (UNIV_UNLIKELY(!success)) {
		mutex_enter(&block->mutex);
		buf_block_buf_fix_dec(block);
		mutex_exit(&block->mutex);

		return(FALSE);
	}

	if (UNIV_UNLIKELY(modify_clock != block->modify_clock)) {
#ifdef UNIV_SYNC_DEBUG
		buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);
#endif /* UNIV_SYNC_DEBUG */
		if (rw_latch == RW_S_LATCH) {
			rw_lock_s_unlock(&(block->lock));
		} else {
			rw_lock_x_unlock(&(block->lock));
		}

		mutex_enter(&block->mutex);
		buf_block_buf_fix_dec(block);
		mutex_exit(&block->mutex);

		return(FALSE);
	}

	mtr_memo_push(mtr, block, fix_type);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
	ut_a(block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

#ifdef UNIV_DEBUG_FILE_ACCESSES
	ut_a(block->page.file_page_was_freed == FALSE);
#endif
	if (UNIV_UNLIKELY(!accessed)) {
		/* In the case of a first access, try to apply linear
		read-ahead */

		buf_read_ahead_linear(buf_block_get_space(block),
				      buf_block_get_zip_size(block),
				      buf_block_get_page_no(block));
	}

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(buf_block_get_space(block),
			    buf_block_get_page_no(block)) == 0);
#endif
	buf_pool->n_page_gets++;

	return(TRUE);
}

/************************************************************************
This is used to get access to a known database page, when no waiting can be
done. For example, if a search in an adaptive hash index leads us to this
frame. */
UNIV_INTERN
ibool
buf_page_get_known_nowait(
/*======================*/
				/* out: TRUE if success */
	ulint		rw_latch,/* in: RW_S_LATCH, RW_X_LATCH */
	buf_block_t*	block,	/* in: the known page */
	ulint		mode,	/* in: BUF_MAKE_YOUNG or BUF_KEEP_OLD */
	const char*	file,	/* in: file name */
	ulint		line,	/* in: line where called */
	mtr_t*		mtr)	/* in: mini-transaction */
{
	ibool		success;
	ulint		fix_type;

	ut_ad(mtr);
	ut_ad((rw_latch == RW_S_LATCH) || (rw_latch == RW_X_LATCH));

	mutex_enter(&block->mutex);

	if (buf_block_get_state(block) == BUF_BLOCK_REMOVE_HASH) {
		/* Another thread is just freeing the block from the LRU list
		of the buffer pool: do not try to access this page; this
		attempt to access the page can only come through the hash
		index because when the buffer block state is ..._REMOVE_HASH,
		we have already removed it from the page address hash table
		of the buffer pool. */

		mutex_exit(&block->mutex);

		return(FALSE);
	}

	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

	buf_block_buf_fix_inc(block, file, line);

	mutex_exit(&block->mutex);

	if (mode == BUF_MAKE_YOUNG) {
		buf_block_make_young(&block->page);
	}

	ut_ad(!ibuf_inside() || (mode == BUF_KEEP_OLD));

	if (rw_latch == RW_S_LATCH) {
		success = rw_lock_s_lock_func_nowait(&(block->lock),
						     file, line);
		fix_type = MTR_MEMO_PAGE_S_FIX;
	} else {
		success = rw_lock_x_lock_func_nowait(&(block->lock),
						     file, line);
		fix_type = MTR_MEMO_PAGE_X_FIX;
	}

	if (!success) {
		mutex_enter(&block->mutex);
		buf_block_buf_fix_dec(block);
		mutex_exit(&block->mutex);

		return(FALSE);
	}

	mtr_memo_push(mtr, block, fix_type);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
	ut_a(block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#ifdef UNIV_DEBUG_FILE_ACCESSES
	ut_a(block->page.file_page_was_freed == FALSE);
#endif

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a((mode == BUF_KEEP_OLD)
	     || (ibuf_count_get(buf_block_get_space(block),
				buf_block_get_page_no(block)) == 0));
#endif
	buf_pool->n_page_gets++;

	return(TRUE);
}

/***********************************************************************
Given a tablespace id and page number tries to get that page. If the
page is not in the buffer pool it is not loaded and NULL is returned.
Suitable for using when holding the kernel mutex. */
UNIV_INTERN
const buf_block_t*
buf_page_try_get_func(
/*==================*/
				/* out: pointer to a page or NULL */
	ulint		space_id,/* in: tablespace id */
	ulint		page_no,/* in: page number */
	const char*	file,	/* in: file name */
	ulint		line,	/* in: line where called */
	mtr_t*		mtr)	/* in: mini-transaction */
{
	buf_block_t*	block;
	ibool		success;
	ulint		fix_type;

	buf_pool_mutex_enter();
	block = buf_block_hash_get(space_id, page_no);

	if (!block) {
		buf_pool_mutex_exit();
		return(NULL);
	}

	mutex_enter(&block->mutex);
	buf_pool_mutex_exit();

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_a(buf_block_get_space(block) == space_id);
	ut_a(buf_block_get_page_no(block) == page_no);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	buf_block_buf_fix_inc(block, file, line);
	mutex_exit(&block->mutex);

	fix_type = MTR_MEMO_PAGE_S_FIX;
	success = rw_lock_s_lock_func_nowait(&block->lock, file, line);

	if (!success) {
		/* Let us try to get an X-latch. If the current thread
		is holding an X-latch on the page, we cannot get an
		S-latch. */

		fix_type = MTR_MEMO_PAGE_X_FIX;
		success = rw_lock_x_lock_func_nowait(&block->lock,
						     file, line);
	}

	if (!success) {
		mutex_enter(&block->mutex);
		buf_block_buf_fix_dec(block);
		mutex_exit(&block->mutex);

		return(NULL);
	}

	mtr_memo_push(mtr, block, fix_type);
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
	ut_a(block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#ifdef UNIV_DEBUG_FILE_ACCESSES
	ut_a(block->page.file_page_was_freed == FALSE);
#endif /* UNIV_DEBUG_FILE_ACCESSES */
#ifdef UNIV_SYNC_DEBUG
	buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);
#endif /* UNIV_SYNC_DEBUG */
	buf_pool->n_page_gets++;

	return(block);
}

/************************************************************************
Initialize some fields of a control block. */
UNIV_INLINE
void
buf_page_init_low(
/*==============*/
	buf_page_t*	bpage)	/* in: block to init */
{
	bpage->flush_type = BUF_FLUSH_LRU;
	bpage->accessed = FALSE;
	bpage->io_fix = BUF_IO_NONE;
	bpage->buf_fix_count = 0;
	bpage->freed_page_clock = 0;
	bpage->newest_modification = 0;
	bpage->oldest_modification = 0;
	HASH_INVALIDATE(bpage, hash);
#ifdef UNIV_DEBUG_FILE_ACCESSES
	bpage->file_page_was_freed = FALSE;
#endif /* UNIV_DEBUG_FILE_ACCESSES */
}

#ifdef UNIV_HOTBACKUP
/************************************************************************
Inits a page to the buffer buf_pool, for use in ibbackup --restore. */
UNIV_INTERN
void
buf_page_init_for_backup_restore(
/*=============================*/
	ulint		space,	/* in: space id */
	ulint		offset,	/* in: offset of the page within space
				in units of a page */
	ulint		zip_size,/* in: compressed page size in bytes
				or 0 for uncompressed pages */
	buf_block_t*	block)	/* in: block to init */
{
	buf_block_init_low(block);

	block->lock_hash_val	= 0;

	buf_page_init_low(&block->page);
	block->page.state	= BUF_BLOCK_FILE_PAGE;
	block->page.space	= space;
	block->page.offset	= offset;

	page_zip_des_init(&block->page.zip);

	/* We assume that block->page.data has been allocated
	with zip_size == UNIV_PAGE_SIZE. */
	ut_ad(zip_size <= UNIV_PAGE_SIZE);
	ut_ad(ut_is_2pow(zip_size));
	page_zip_set_size(&block->page.zip, zip_size);
}
#endif /* UNIV_HOTBACKUP */

/************************************************************************
Inits a page to the buffer buf_pool. */
static
void
buf_page_init(
/*==========*/
	ulint		space,	/* in: space id */
	ulint		offset,	/* in: offset of the page within space
				in units of a page */
	buf_block_t*	block)	/* in: block to init */
{
	buf_page_t*	hash_page;

	ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(&(block->mutex)));
	ut_a(buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE);

	/* Set the state of the block */
	buf_block_set_file_page(block, space, offset);

#ifdef UNIV_DEBUG_VALGRIND
	if (!space) {
		/* Silence valid Valgrind warnings about uninitialized
		data being written to data files.  There are some unused
		bytes on some pages that InnoDB does not initialize. */
		UNIV_MEM_VALID(block->frame, UNIV_PAGE_SIZE);
	}
#endif /* UNIV_DEBUG_VALGRIND */

	buf_block_init_low(block);

	block->lock_hash_val	= lock_rec_hash(space, offset);

	/* Insert into the hash table of file pages */

	hash_page = buf_page_hash_get(space, offset);

	if (UNIV_LIKELY_NULL(hash_page)) {
		fprintf(stderr,
			"InnoDB: Error: page %lu %lu already found"
			" in the hash table: %p, %p\n",
			(ulong) space,
			(ulong) offset,
			(const void*) hash_page, (const void*) block);
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		mutex_exit(&block->mutex);
		buf_pool_mutex_exit();
		buf_print();
		buf_LRU_print();
		buf_validate();
		buf_LRU_validate();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		ut_error;
	}

	buf_page_init_low(&block->page);

	ut_ad(!block->page.in_zip_hash);
	ut_ad(!block->page.in_page_hash);
	ut_d(block->page.in_page_hash = TRUE);
	HASH_INSERT(buf_page_t, hash, buf_pool->page_hash,
		    buf_page_address_fold(space, offset), &block->page);
}

/************************************************************************
Function which inits a page for read to the buffer buf_pool. If the page is
(1) already in buf_pool, or
(2) if we specify to read only ibuf pages and the page is not an ibuf page, or
(3) if the space is deleted or being deleted,
then this function does nothing.
Sets the io_fix flag to BUF_IO_READ and sets a non-recursive exclusive lock
on the buffer frame. The io-handler must take care that the flag is cleared
and the lock released later. */
UNIV_INTERN
buf_page_t*
buf_page_init_for_read(
/*===================*/
				/* out: pointer to the block or NULL */
	ulint*		err,	/* out: DB_SUCCESS or DB_TABLESPACE_DELETED */
	ulint		mode,	/* in: BUF_READ_IBUF_PAGES_ONLY, ... */
	ulint		space,	/* in: space id */
	ulint		zip_size,/* in: compressed page size, or 0 */
	ibool		unzip,	/* in: TRUE=request uncompressed page */
	ib_longlong	tablespace_version,/* in: prevents reading from a wrong
				version of the tablespace in case we have done
				DISCARD + IMPORT */
	ulint		offset)	/* in: page number */
{
	buf_block_t*	block;
	buf_page_t*	bpage;
	mtr_t		mtr;
	ibool		lru	= FALSE;
	void*		data;

	ut_ad(buf_pool);

	*err = DB_SUCCESS;

	if (mode == BUF_READ_IBUF_PAGES_ONLY) {
		/* It is a read-ahead within an ibuf routine */

		ut_ad(!ibuf_bitmap_page(zip_size, offset));
		ut_ad(ibuf_inside());

		mtr_start(&mtr);

		if (!ibuf_page_low(space, zip_size, offset, &mtr)) {

			mtr_commit(&mtr);

			return(NULL);
		}
	} else {
		ut_ad(mode == BUF_READ_ANY_PAGE);
	}

	if (zip_size && UNIV_LIKELY(!unzip)
	    && UNIV_LIKELY(!recv_recovery_is_on())) {
		block = NULL;
	} else {
		block = buf_LRU_get_free_block(0);
		ut_ad(block);
	}

	buf_pool_mutex_enter();

	if (buf_page_hash_get(space, offset)) {
		/* The page is already in the buffer pool. */
err_exit:
		if (block) {
			mutex_enter(&block->mutex);
			buf_LRU_block_free_non_file_page(block);
			mutex_exit(&block->mutex);
		}

err_exit2:
		buf_pool_mutex_exit();

		if (mode == BUF_READ_IBUF_PAGES_ONLY) {

			mtr_commit(&mtr);
		}

		return(NULL);
	}

	if (fil_tablespace_deleted_or_being_deleted_in_mem(
		    space, tablespace_version)) {
		/* The page belongs to a space which has been
		deleted or is being deleted. */
		*err = DB_TABLESPACE_DELETED;

		goto err_exit;
	}

	if (block) {
		bpage = &block->page;
		mutex_enter(&block->mutex);
		buf_page_init(space, offset, block);

		/* The block must be put to the LRU list, to the old blocks */
		buf_LRU_add_block(bpage, TRUE/* to old blocks */);

		/* We set a pass-type x-lock on the frame because then
		the same thread which called for the read operation
		(and is running now at this point of code) can wait
		for the read to complete by waiting for the x-lock on
		the frame; if the x-lock were recursive, the same
		thread would illegally get the x-lock before the page
		read is completed.  The x-lock is cleared by the
		io-handler thread. */

		rw_lock_x_lock_gen(&block->lock, BUF_IO_READ);
		buf_page_set_io_fix(bpage, BUF_IO_READ);

		if (UNIV_UNLIKELY(zip_size)) {
			page_zip_set_size(&block->page.zip, zip_size);

			/* buf_pool_mutex may be released and
			reacquired by buf_buddy_alloc().  Thus, we
			must release block->mutex in order not to
			break the latching order in the reacquisition
			of buf_pool_mutex.  We also must defer this
			operation until after the block descriptor has
			been added to buf_pool->LRU and
			buf_pool->page_hash. */
			mutex_exit(&block->mutex);
			data = buf_buddy_alloc(zip_size, &lru);
			mutex_enter(&block->mutex);
			block->page.zip.data = data;
		}

		mutex_exit(&block->mutex);
	} else {
		/* Defer buf_buddy_alloc() until after the block has
		been found not to exist.  The buf_buddy_alloc() and
		buf_buddy_free() calls may be expensive because of
		buf_buddy_relocate(). */

		/* The compressed page must be allocated before the
		control block (bpage), in order to avoid the
		invocation of buf_buddy_relocate_block() on
		uninitialized data. */
		data = buf_buddy_alloc(zip_size, &lru);
		bpage = buf_buddy_alloc(sizeof *bpage, &lru);

		/* If buf_buddy_alloc() allocated storage from the LRU list,
		it released and reacquired buf_pool_mutex.  Thus, we must
		check the page_hash again, as it may have been modified. */
		if (UNIV_UNLIKELY(lru)
		    && UNIV_LIKELY_NULL(buf_page_hash_get(space, offset))) {

			/* The block was added by some other thread. */
			buf_buddy_free(bpage, sizeof *bpage);
			buf_buddy_free(data, zip_size);
			goto err_exit2;
		}

		page_zip_des_init(&bpage->zip);
		page_zip_set_size(&bpage->zip, zip_size);
		bpage->zip.data = data;

		mutex_enter(&buf_pool_zip_mutex);
		UNIV_MEM_DESC(bpage->zip.data,
			      page_zip_get_size(&bpage->zip), bpage);
		buf_page_init_low(bpage);
		bpage->state	= BUF_BLOCK_ZIP_PAGE;
		bpage->space	= space;
		bpage->offset	= offset;

#ifdef UNIV_DEBUG
		bpage->in_page_hash = FALSE;
		bpage->in_zip_hash = FALSE;
		bpage->in_flush_list = FALSE;
		bpage->in_free_list = FALSE;
		bpage->in_LRU_list = FALSE;
#endif /* UNIV_DEBUG */

		ut_d(bpage->in_page_hash = TRUE);
		HASH_INSERT(buf_page_t, hash, buf_pool->page_hash,
			    buf_page_address_fold(space, offset), bpage);

		/* The block must be put to the LRU list, to the old blocks */
		buf_LRU_add_block(bpage, TRUE/* to old blocks */);
		buf_LRU_insert_zip_clean(bpage);

		buf_page_set_io_fix(bpage, BUF_IO_READ);

		mutex_exit(&buf_pool_zip_mutex);
	}

	buf_pool->n_pend_reads++;
	buf_pool_mutex_exit();

	if (mode == BUF_READ_IBUF_PAGES_ONLY) {

		mtr_commit(&mtr);
	}

	ut_ad(buf_page_in_file(bpage));
	return(bpage);
}

/************************************************************************
Initializes a page to the buffer buf_pool. The page is usually not read
from a file even if it cannot be found in the buffer buf_pool. This is one
of the functions which perform to a block a state transition NOT_USED =>
FILE_PAGE (the other is buf_page_get_gen). */
UNIV_INTERN
buf_block_t*
buf_page_create(
/*============*/
			/* out: pointer to the block, page bufferfixed */
	ulint	space,	/* in: space id */
	ulint	offset,	/* in: offset of the page within space in units of
			a page */
	ulint	zip_size,/* in: compressed page size, or 0 */
	mtr_t*	mtr)	/* in: mini-transaction handle */
{
	buf_frame_t*	frame;
	buf_block_t*	block;
	buf_block_t*	free_block	= NULL;

	ut_ad(mtr);
	ut_ad(space || !zip_size);

	free_block = buf_LRU_get_free_block(0);

	buf_pool_mutex_enter();

	block = (buf_block_t*) buf_page_hash_get(space, offset);

	if (block && buf_page_in_file(&block->page)) {
#ifdef UNIV_IBUF_COUNT_DEBUG
		ut_a(ibuf_count_get(space, offset) == 0);
#endif
#ifdef UNIV_DEBUG_FILE_ACCESSES
		block->page.file_page_was_freed = FALSE;
#endif /* UNIV_DEBUG_FILE_ACCESSES */

		/* Page can be found in buf_pool */
		buf_pool_mutex_exit();

		buf_block_free(free_block);

		return(buf_page_get_with_no_latch(space, zip_size,
						  offset, mtr));
	}

	/* If we get here, the page was not in buf_pool: init it there */

#ifdef UNIV_DEBUG
	if (buf_debug_prints) {
		fprintf(stderr, "Creating space %lu page %lu to buffer\n",
			(ulong) space, (ulong) offset);
	}
#endif /* UNIV_DEBUG */

	block = free_block;

	mutex_enter(&block->mutex);

	buf_page_init(space, offset, block);

	/* The block must be put to the LRU list */
	buf_LRU_add_block(&block->page, FALSE);

	buf_block_buf_fix_inc(block, __FILE__, __LINE__);
	buf_pool->n_pages_created++;

	if (zip_size) {
		void*	data;
		ibool	lru;

		/* Prevent race conditions during buf_buddy_alloc(),
		which may release and reacquire buf_pool_mutex,
		by IO-fixing and X-latching the block. */

		buf_page_set_io_fix(&block->page, BUF_IO_READ);
		rw_lock_x_lock(&block->lock);

		page_zip_set_size(&block->page.zip, zip_size);
		mutex_exit(&block->mutex);
		/* buf_pool_mutex may be released and reacquired by
		buf_buddy_alloc().  Thus, we must release block->mutex
		in order not to break the latching order in
		the reacquisition of buf_pool_mutex.  We also must
		defer this operation until after the block descriptor
		has been added to buf_pool->LRU and buf_pool->page_hash. */
		data = buf_buddy_alloc(zip_size, &lru);
		mutex_enter(&block->mutex);
		block->page.zip.data = data;

		buf_page_set_io_fix(&block->page, BUF_IO_NONE);
		rw_lock_x_unlock(&block->lock);
	}

	buf_pool_mutex_exit();

	mtr_memo_push(mtr, block, MTR_MEMO_BUF_FIX);

	buf_page_set_accessed(&block->page, TRUE);

	mutex_exit(&block->mutex);

	/* Delete possible entries for the page from the insert buffer:
	such can exist if the page belonged to an index which was dropped */

	ibuf_merge_or_delete_for_page(NULL, space, offset, zip_size, TRUE);

	/* Flush pages from the end of the LRU list if necessary */
	buf_flush_free_margin();

	frame = block->frame;

	memset(frame + FIL_PAGE_PREV, 0xff, 4);
	memset(frame + FIL_PAGE_NEXT, 0xff, 4);
	mach_write_to_2(frame + FIL_PAGE_TYPE, FIL_PAGE_TYPE_ALLOCATED);

	/* Reset to zero the file flush lsn field in the page; if the first
	page of an ibdata file is 'created' in this function into the buffer
	pool then we lose the original contents of the file flush lsn stamp.
	Then InnoDB could in a crash recovery print a big, false, corruption
	warning if the stamp contains an lsn bigger than the ib_logfile lsn. */

	memset(frame + FIL_PAGE_FILE_FLUSH_LSN, 0, 8);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 357 || buf_validate());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(buf_block_get_space(block),
			    buf_block_get_page_no(block)) == 0);
#endif
	return(block);
}

/************************************************************************
Completes an asynchronous read or write request of a file page to or from
the buffer pool. */
UNIV_INTERN
void
buf_page_io_complete(
/*=================*/
	buf_page_t*	bpage)	/* in: pointer to the block in question */
{
	enum buf_io_fix	io_type;
	const ibool	uncompressed = (buf_page_get_state(bpage)
					== BUF_BLOCK_FILE_PAGE);

	ut_a(buf_page_in_file(bpage));

	/* We do not need protect io_fix here by mutex to read
	it because this is the only function where we can change the value
	from BUF_IO_READ or BUF_IO_WRITE to some other value, and our code
	ensures that this is the only thread that handles the i/o for this
	block. */

	io_type = buf_page_get_io_fix(bpage);
	ut_ad(io_type == BUF_IO_READ || io_type == BUF_IO_WRITE);

	if (io_type == BUF_IO_READ) {
		ulint	read_page_no;
		ulint	read_space_id;
		byte*	frame;

		if (buf_page_get_zip_size(bpage)) {
			frame = bpage->zip.data;
			buf_pool->n_pend_unzip++;
			if (uncompressed
			    && !buf_zip_decompress((buf_block_t*) bpage,
						   FALSE)) {

				buf_pool->n_pend_unzip--;
				goto corrupt;
			}
			buf_pool->n_pend_unzip--;
		} else {
			ut_a(uncompressed);
			frame = ((buf_block_t*) bpage)->frame;
		}

		/* If this page is not uninitialized and not in the
		doublewrite buffer, then the page number and space id
		should be the same as in block. */
		read_page_no = mach_read_from_4(frame + FIL_PAGE_OFFSET);
		read_space_id = mach_read_from_4(
			frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);

		if (bpage->space == TRX_SYS_SPACE
		    && trx_doublewrite_page_inside(bpage->offset)) {

			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: Error: reading page %lu\n"
				"InnoDB: which is in the"
				" doublewrite buffer!\n",
				(ulong) bpage->offset);
		} else if (!read_space_id && !read_page_no) {
			/* This is likely an uninitialized page. */
		} else if ((bpage->space
			    && bpage->space != read_space_id)
			   || bpage->offset != read_page_no) {
			/* We did not compare space_id to read_space_id
			if bpage->space == 0, because the field on the
			page may contain garbage in MySQL < 4.1.1,
			which only supported bpage->space == 0. */

			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: Error: space id and page n:o"
				" stored in the page\n"
				"InnoDB: read in are %lu:%lu,"
				" should be %lu:%lu!\n",
				(ulong) read_space_id, (ulong) read_page_no,
				(ulong) bpage->space,
				(ulong) bpage->offset);
		}

		/* From version 3.23.38 up we store the page checksum
		to the 4 first bytes of the page end lsn field */

		if (buf_page_is_corrupted(frame,
					  buf_page_get_zip_size(bpage))) {
corrupt:
			fprintf(stderr,
				"InnoDB: Database page corruption on disk"
				" or a failed\n"
				"InnoDB: file read of page %lu.\n"
				"InnoDB: You may have to recover"
				" from a backup.\n",
				(ulong) bpage->offset);
			buf_page_print(frame, buf_page_get_zip_size(bpage));
			fprintf(stderr,
				"InnoDB: Database page corruption on disk"
				" or a failed\n"
				"InnoDB: file read of page %lu.\n"
				"InnoDB: You may have to recover"
				" from a backup.\n",
				(ulong) bpage->offset);
			fputs("InnoDB: It is also possible that"
			      " your operating\n"
			      "InnoDB: system has corrupted its"
			      " own file cache\n"
			      "InnoDB: and rebooting your computer"
			      " removes the\n"
			      "InnoDB: error.\n"
			      "InnoDB: If the corrupt page is an index page\n"
			      "InnoDB: you can also try to"
			      " fix the corruption\n"
			      "InnoDB: by dumping, dropping,"
			      " and reimporting\n"
			      "InnoDB: the corrupt table."
			      " You can use CHECK\n"
			      "InnoDB: TABLE to scan your"
			      " table for corruption.\n"
			      "InnoDB: See also"
			      " http://dev.mysql.com/doc/refman/5.1/en/"
			      "forcing-recovery.html\n"
			      "InnoDB: about forcing recovery.\n", stderr);

			if (srv_force_recovery < SRV_FORCE_IGNORE_CORRUPT) {
				fputs("InnoDB: Ending processing because of"
				      " a corrupt database page.\n",
				      stderr);
				exit(1);
			}
		}

		if (recv_recovery_is_on()) {
			/* Pages must be uncompressed for crash recovery. */
			ut_a(uncompressed);
			recv_recover_page(FALSE, TRUE, (buf_block_t*) bpage);
		}

		if (uncompressed && !recv_no_ibuf_operations) {
			ibuf_merge_or_delete_for_page(
				(buf_block_t*) bpage, bpage->space,
				bpage->offset, buf_page_get_zip_size(bpage),
				TRUE);
		}
	}

	buf_pool_mutex_enter();
	mutex_enter(buf_page_get_mutex(bpage));

#ifdef UNIV_IBUF_COUNT_DEBUG
	if (io_type == BUF_IO_WRITE || uncompressed) {
		/* For BUF_IO_READ of compressed-only blocks, the
		buffered operations will be merged by buf_page_get_gen()
		after the block has been uncompressed. */
		ut_a(ibuf_count_get(bpage->space, bpage->offset) == 0);
	}
#endif
	/* Because this thread which does the unlocking is not the same that
	did the locking, we use a pass value != 0 in unlock, which simply
	removes the newest lock debug record, without checking the thread
	id. */

	buf_page_set_io_fix(bpage, BUF_IO_NONE);

	switch (io_type) {
	case BUF_IO_READ:
		/* NOTE that the call to ibuf may have moved the ownership of
		the x-latch to this OS thread: do not let this confuse you in
		debugging! */

		ut_ad(buf_pool->n_pend_reads > 0);
		buf_pool->n_pend_reads--;
		buf_pool->n_pages_read++;

		if (uncompressed) {
			rw_lock_x_unlock_gen(&((buf_block_t*) bpage)->lock,
					     BUF_IO_READ);
		}

		break;

	case BUF_IO_WRITE:
		/* Write means a flush operation: call the completion
		routine in the flush system */

		buf_flush_write_complete(bpage);

		if (uncompressed) {
			rw_lock_s_unlock_gen(&((buf_block_t*) bpage)->lock,
					     BUF_IO_WRITE);
		}

		buf_pool->n_pages_written++;

		break;

	default:
		ut_error;
	}

	mutex_exit(buf_page_get_mutex(bpage));
	buf_pool_mutex_exit();

#ifdef UNIV_DEBUG
	if (buf_debug_prints) {
		fprintf(stderr, "Has %s page space %lu page no %lu\n",
			io_type == BUF_IO_READ ? "read" : "written",
			(ulong) buf_page_get_space(bpage),
			(ulong) buf_page_get_page_no(bpage));
	}
#endif /* UNIV_DEBUG */
}

/*************************************************************************
Invalidates the file pages in the buffer pool when an archive recovery is
completed. All the file pages buffered must be in a replaceable state when
this function is called: not latched and not modified. */
UNIV_INTERN
void
buf_pool_invalidate(void)
/*=====================*/
{
	ibool	freed;

	ut_ad(buf_all_freed());

	freed = TRUE;

	while (freed) {
		freed = buf_LRU_search_and_free_block(100);
	}

	buf_pool_mutex_enter();

	ut_ad(UT_LIST_GET_LEN(buf_pool->LRU) == 0);

	buf_pool_mutex_exit();
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/*************************************************************************
Validates the buffer buf_pool data structure. */
UNIV_INTERN
ibool
buf_validate(void)
/*==============*/
{
	buf_page_t*	b;
	buf_chunk_t*	chunk;
	ulint		i;
	ulint		n_single_flush	= 0;
	ulint		n_lru_flush	= 0;
	ulint		n_list_flush	= 0;
	ulint		n_lru		= 0;
	ulint		n_flush		= 0;
	ulint		n_free		= 0;
	ulint		n_zip		= 0;

	ut_ad(buf_pool);

	buf_pool_mutex_enter();

	chunk = buf_pool->chunks;

	/* Check the uncompressed blocks. */

	for (i = buf_pool->n_chunks; i--; chunk++) {

		ulint		j;
		buf_block_t*	block = chunk->blocks;

		for (j = chunk->size; j--; block++) {

			mutex_enter(&block->mutex);

			switch (buf_block_get_state(block)) {
			case BUF_BLOCK_ZIP_FREE:
			case BUF_BLOCK_ZIP_PAGE:
			case BUF_BLOCK_ZIP_DIRTY:
				/* These should only occur on
				zip_clean, zip_free[], or flush_list. */
				ut_error;
				break;

			case BUF_BLOCK_FILE_PAGE:
				ut_a(buf_page_hash_get(buf_block_get_space(
							       block),
						       buf_block_get_page_no(
							       block))
				     == &block->page);

#ifdef UNIV_IBUF_COUNT_DEBUG
				ut_a(buf_page_get_io_fix(&block->page)
				     == BUF_IO_READ
				     || !ibuf_count_get(buf_block_get_space(
								block),
							buf_block_get_page_no(
								block)));
#endif
				switch (buf_page_get_io_fix(&block->page)) {
				case BUF_IO_NONE:
					break;

				case BUF_IO_WRITE:
					switch (buf_page_get_flush_type(
							&block->page)) {
					case BUF_FLUSH_LRU:
						n_lru_flush++;
						ut_a(rw_lock_is_locked(
							     &block->lock,
							     RW_LOCK_SHARED));
						break;
					case BUF_FLUSH_LIST:
						n_list_flush++;
						break;
					case BUF_FLUSH_SINGLE_PAGE:
						n_single_flush++;
						break;
					default:
						ut_error;
					}

					break;

				case BUF_IO_READ:

					ut_a(rw_lock_is_locked(&block->lock,
							       RW_LOCK_EX));
					break;
				}

				n_lru++;

				if (block->page.oldest_modification > 0) {
					n_flush++;
				}

				break;

			case BUF_BLOCK_NOT_USED:
				n_free++;
				break;

			case BUF_BLOCK_READY_FOR_USE:
			case BUF_BLOCK_MEMORY:
			case BUF_BLOCK_REMOVE_HASH:
				/* do nothing */
				break;
			}

			mutex_exit(&block->mutex);
		}
	}

	mutex_enter(&buf_pool_zip_mutex);

	/* Check clean compressed-only blocks. */

	for (b = UT_LIST_GET_FIRST(buf_pool->zip_clean); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_a(buf_page_get_state(b) == BUF_BLOCK_ZIP_PAGE);
		switch (buf_page_get_io_fix(b)) {
		case BUF_IO_NONE:
			/* All clean blocks should be I/O-unfixed. */
			break;
		case BUF_IO_READ:
			/* In buf_LRU_free_block(), we temporarily set
			b->io_fix = BUF_IO_READ for a newly allocated
			control block in order to prevent
			buf_page_get_gen() from decompressing the block. */
			break;
		default:
			ut_error;
			break;
		}
		ut_a(!b->oldest_modification);
		ut_a(buf_page_hash_get(b->space, b->offset) == b);

		n_lru++;
		n_zip++;
	}

	/* Check dirty compressed-only blocks. */

	for (b = UT_LIST_GET_FIRST(buf_pool->flush_list); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_ad(b->in_flush_list);

		switch (buf_page_get_state(b)) {
		case BUF_BLOCK_ZIP_DIRTY:
			ut_a(b->oldest_modification);
			n_lru++;
			n_flush++;
			n_zip++;
			switch (buf_page_get_io_fix(b)) {
			case BUF_IO_NONE:
			case BUF_IO_READ:
				break;

			case BUF_IO_WRITE:
				switch (buf_page_get_flush_type(b)) {
				case BUF_FLUSH_LRU:
					n_lru_flush++;
					break;
				case BUF_FLUSH_LIST:
					n_list_flush++;
					break;
				case BUF_FLUSH_SINGLE_PAGE:
					n_single_flush++;
					break;
				default:
					ut_error;
				}
				break;
			}
			break;
		case BUF_BLOCK_FILE_PAGE:
			/* uncompressed page */
			break;
		case BUF_BLOCK_ZIP_FREE:
		case BUF_BLOCK_ZIP_PAGE:
		case BUF_BLOCK_NOT_USED:
		case BUF_BLOCK_READY_FOR_USE:
		case BUF_BLOCK_MEMORY:
		case BUF_BLOCK_REMOVE_HASH:
			ut_error;
			break;
		}
		ut_a(buf_page_hash_get(b->space, b->offset) == b);
	}

	mutex_exit(&buf_pool_zip_mutex);

	if (n_lru + n_free > buf_pool->curr_size + n_zip) {
		fprintf(stderr, "n LRU %lu, n free %lu, pool %lu zip %lu\n",
			(ulong) n_lru, (ulong) n_free,
			(ulong) buf_pool->curr_size, (ulong) n_zip);
		ut_error;
	}

	ut_a(UT_LIST_GET_LEN(buf_pool->LRU) == n_lru);
	if (UT_LIST_GET_LEN(buf_pool->free) != n_free) {
		fprintf(stderr, "Free list len %lu, free blocks %lu\n",
			(ulong) UT_LIST_GET_LEN(buf_pool->free),
			(ulong) n_free);
		ut_error;
	}
	ut_a(UT_LIST_GET_LEN(buf_pool->flush_list) == n_flush);

	ut_a(buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE] == n_single_flush);
	ut_a(buf_pool->n_flush[BUF_FLUSH_LIST] == n_list_flush);
	ut_a(buf_pool->n_flush[BUF_FLUSH_LRU] == n_lru_flush);

	buf_pool_mutex_exit();

	ut_a(buf_LRU_validate());
	ut_a(buf_flush_validate());

	return(TRUE);
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/*************************************************************************
Prints info of the buffer buf_pool data structure. */
UNIV_INTERN
void
buf_print(void)
/*===========*/
{
	dulint*		index_ids;
	ulint*		counts;
	ulint		size;
	ulint		i;
	ulint		j;
	dulint		id;
	ulint		n_found;
	buf_chunk_t*	chunk;
	dict_index_t*	index;

	ut_ad(buf_pool);

	size = buf_pool->curr_size;

	index_ids = mem_alloc(sizeof(dulint) * size);
	counts = mem_alloc(sizeof(ulint) * size);

	buf_pool_mutex_enter();

	fprintf(stderr,
		"buf_pool size %lu\n"
		"database pages %lu\n"
		"free pages %lu\n"
		"modified database pages %lu\n"
		"n pending decompressions %lu\n"
		"n pending reads %lu\n"
		"n pending flush LRU %lu list %lu single page %lu\n"
		"pages read %lu, created %lu, written %lu\n",
		(ulong) size,
		(ulong) UT_LIST_GET_LEN(buf_pool->LRU),
		(ulong) UT_LIST_GET_LEN(buf_pool->free),
		(ulong) UT_LIST_GET_LEN(buf_pool->flush_list),
		(ulong) buf_pool->n_pend_unzip,
		(ulong) buf_pool->n_pend_reads,
		(ulong) buf_pool->n_flush[BUF_FLUSH_LRU],
		(ulong) buf_pool->n_flush[BUF_FLUSH_LIST],
		(ulong) buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE],
		(ulong) buf_pool->n_pages_read, buf_pool->n_pages_created,
		(ulong) buf_pool->n_pages_written);

	/* Count the number of blocks belonging to each index in the buffer */

	n_found = 0;

	chunk = buf_pool->chunks;

	for (i = buf_pool->n_chunks; i--; chunk++) {
		buf_block_t*	block		= chunk->blocks;
		ulint		n_blocks	= chunk->size;

		for (; n_blocks--; block++) {
			const buf_frame_t* frame = block->frame;

			if (fil_page_get_type(frame) == FIL_PAGE_INDEX) {

				id = btr_page_get_index_id(frame);

				/* Look for the id in the index_ids array */
				j = 0;

				while (j < n_found) {

					if (ut_dulint_cmp(index_ids[j],
							  id) == 0) {
						counts[j]++;

						break;
					}
					j++;
				}

				if (j == n_found) {
					n_found++;
					index_ids[j] = id;
					counts[j] = 1;
				}
			}
		}
	}

	buf_pool_mutex_exit();

	for (i = 0; i < n_found; i++) {
		index = dict_index_get_if_in_cache(index_ids[i]);

		fprintf(stderr,
			"Block count for index %lu in buffer is about %lu",
			(ulong) ut_dulint_get_low(index_ids[i]),
			(ulong) counts[i]);

		if (index) {
			putc(' ', stderr);
			dict_index_name_print(stderr, NULL, index);
		}

		putc('\n', stderr);
	}

	mem_free(index_ids);
	mem_free(counts);

	ut_a(buf_validate());
}
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG || UNIV_BUF_DEBUG */

/*************************************************************************
Returns the number of latched pages in the buffer pool. */
UNIV_INTERN
ulint
buf_get_latched_pages_number(void)
/*==============================*/
{
	buf_chunk_t*	chunk;
	buf_page_t*	b;
	ulint		i;
	ulint		fixed_pages_number = 0;

	buf_pool_mutex_enter();

	chunk = buf_pool->chunks;

	for (i = buf_pool->n_chunks; i--; chunk++) {
		buf_block_t*	block;
		ulint		j;

		block = chunk->blocks;

		for (j = chunk->size; j--; block++) {
			if (buf_block_get_state(block)
			    != BUF_BLOCK_FILE_PAGE) {

				continue;
			}

			mutex_enter(&block->mutex);

			if (block->page.buf_fix_count != 0
			    || buf_page_get_io_fix(&block->page)
			    != BUF_IO_NONE) {
				fixed_pages_number++;
			}

			mutex_exit(&block->mutex);
		}
	}

	mutex_enter(&buf_pool_zip_mutex);

	/* Traverse the lists of clean and dirty compressed-only blocks. */

	for (b = UT_LIST_GET_FIRST(buf_pool->zip_clean); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_a(buf_page_get_state(b) == BUF_BLOCK_ZIP_PAGE);
		ut_a(buf_page_get_io_fix(b) == BUF_IO_NONE);

		if (b->buf_fix_count != 0
		    || buf_page_get_io_fix(b) != BUF_IO_NONE) {
			fixed_pages_number++;
		}
	}

	for (b = UT_LIST_GET_FIRST(buf_pool->flush_list); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_ad(b->in_flush_list);

		switch (buf_page_get_state(b)) {
		case BUF_BLOCK_ZIP_DIRTY:
			if (b->buf_fix_count != 0
			    || buf_page_get_io_fix(b) != BUF_IO_NONE) {
				fixed_pages_number++;
			}
			break;
		case BUF_BLOCK_FILE_PAGE:
			/* uncompressed page */
			break;
		case BUF_BLOCK_ZIP_FREE:
		case BUF_BLOCK_ZIP_PAGE:
		case BUF_BLOCK_NOT_USED:
		case BUF_BLOCK_READY_FOR_USE:
		case BUF_BLOCK_MEMORY:
		case BUF_BLOCK_REMOVE_HASH:
			ut_error;
			break;
		}
	}

	mutex_exit(&buf_pool_zip_mutex);
	buf_pool_mutex_exit();

	return(fixed_pages_number);
}

/*************************************************************************
Returns the number of pending buf pool ios. */
UNIV_INTERN
ulint
buf_get_n_pending_ios(void)
/*=======================*/
{
	return(buf_pool->n_pend_reads
	       + buf_pool->n_flush[BUF_FLUSH_LRU]
	       + buf_pool->n_flush[BUF_FLUSH_LIST]
	       + buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE]);
}

/*************************************************************************
Returns the ratio in percents of modified pages in the buffer pool /
database pages in the buffer pool. */
UNIV_INTERN
ulint
buf_get_modified_ratio_pct(void)
/*============================*/
{
	ulint	ratio;

	buf_pool_mutex_enter();

	ratio = (100 * UT_LIST_GET_LEN(buf_pool->flush_list))
		/ (1 + UT_LIST_GET_LEN(buf_pool->LRU)
		   + UT_LIST_GET_LEN(buf_pool->free));

	/* 1 + is there to avoid division by zero */

	buf_pool_mutex_exit();

	return(ratio);
}

/*************************************************************************
Prints info of the buffer i/o. */
UNIV_INTERN
void
buf_print_io(
/*=========*/
	FILE*	file)	/* in/out: buffer where to print */
{
	time_t	current_time;
	double	time_elapsed;
	ulint	size;

	ut_ad(buf_pool);
	size = buf_pool->curr_size;

	buf_pool_mutex_enter();

	fprintf(file,
		"Buffer pool size   %lu\n"
		"Free buffers       %lu\n"
		"Database pages     %lu\n"
		"Modified db pages  %lu\n"
		"Pending reads %lu\n"
		"Pending writes: LRU %lu, flush list %lu, single page %lu\n",
		(ulong) size,
		(ulong) UT_LIST_GET_LEN(buf_pool->free),
		(ulong) UT_LIST_GET_LEN(buf_pool->LRU),
		(ulong) UT_LIST_GET_LEN(buf_pool->flush_list),
		(ulong) buf_pool->n_pend_reads,
		(ulong) buf_pool->n_flush[BUF_FLUSH_LRU]
		+ buf_pool->init_flush[BUF_FLUSH_LRU],
		(ulong) buf_pool->n_flush[BUF_FLUSH_LIST]
		+ buf_pool->init_flush[BUF_FLUSH_LIST],
		(ulong) buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE]);

	current_time = time(NULL);
	time_elapsed = 0.001 + difftime(current_time,
					buf_pool->last_printout_time);
	buf_pool->last_printout_time = current_time;

	fprintf(file,
		"Pages read %lu, created %lu, written %lu\n"
		"%.2f reads/s, %.2f creates/s, %.2f writes/s\n",
		(ulong) buf_pool->n_pages_read,
		(ulong) buf_pool->n_pages_created,
		(ulong) buf_pool->n_pages_written,
		(buf_pool->n_pages_read - buf_pool->n_pages_read_old)
		/ time_elapsed,
		(buf_pool->n_pages_created - buf_pool->n_pages_created_old)
		/ time_elapsed,
		(buf_pool->n_pages_written - buf_pool->n_pages_written_old)
		/ time_elapsed);

	if (buf_pool->n_page_gets > buf_pool->n_page_gets_old) {
		fprintf(file, "Buffer pool hit rate %lu / 1000\n",
			(ulong)
			(1000 - ((1000 * (buf_pool->n_pages_read
					  - buf_pool->n_pages_read_old))
				 / (buf_pool->n_page_gets
				    - buf_pool->n_page_gets_old))));
	} else {
		fputs("No buffer pool page gets since the last printout\n",
		      file);
	}

	buf_pool->n_page_gets_old = buf_pool->n_page_gets;
	buf_pool->n_pages_read_old = buf_pool->n_pages_read;
	buf_pool->n_pages_created_old = buf_pool->n_pages_created;
	buf_pool->n_pages_written_old = buf_pool->n_pages_written;

	buf_pool_mutex_exit();
}

/**************************************************************************
Refreshes the statistics used to print per-second averages. */
UNIV_INTERN
void
buf_refresh_io_stats(void)
/*======================*/
{
	buf_pool->last_printout_time = time(NULL);
	buf_pool->n_page_gets_old = buf_pool->n_page_gets;
	buf_pool->n_pages_read_old = buf_pool->n_pages_read;
	buf_pool->n_pages_created_old = buf_pool->n_pages_created;
	buf_pool->n_pages_written_old = buf_pool->n_pages_written;
}

/*************************************************************************
Checks that all file pages in the buffer are in a replaceable state. */
UNIV_INTERN
ibool
buf_all_freed(void)
/*===============*/
{
	buf_chunk_t*	chunk;
	ulint		i;

	ut_ad(buf_pool);

	buf_pool_mutex_enter();

	chunk = buf_pool->chunks;

	for (i = buf_pool->n_chunks; i--; chunk++) {

		const buf_block_t* block = buf_chunk_not_freed(chunk);

		if (UNIV_LIKELY_NULL(block)) {
			fprintf(stderr,
				"Page %lu %lu still fixed or dirty\n",
				(ulong) block->page.space,
				(ulong) block->page.offset);
			ut_error;
		}
	}

	buf_pool_mutex_exit();

	return(TRUE);
}

/*************************************************************************
Checks that there currently are no pending i/o-operations for the buffer
pool. */
UNIV_INTERN
ibool
buf_pool_check_no_pending_io(void)
/*==============================*/
				/* out: TRUE if there is no pending i/o */
{
	ibool	ret;

	buf_pool_mutex_enter();

	if (buf_pool->n_pend_reads + buf_pool->n_flush[BUF_FLUSH_LRU]
	    + buf_pool->n_flush[BUF_FLUSH_LIST]
	    + buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE]) {
		ret = FALSE;
	} else {
		ret = TRUE;
	}

	buf_pool_mutex_exit();

	return(ret);
}

/*************************************************************************
Gets the current length of the free list of buffer blocks. */
UNIV_INTERN
ulint
buf_get_free_list_len(void)
/*=======================*/
{
	ulint	len;

	buf_pool_mutex_enter();

	len = UT_LIST_GET_LEN(buf_pool->free);

	buf_pool_mutex_exit();

	return(len);
}
