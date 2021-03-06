/******************************************************
The interface to the operating system
process control primitives

(c) 1995 Innobase Oy

Created 9/30/1995 Heikki Tuuri
*******************************************************/

#include "os0proc.h"
#ifdef UNIV_NONINL
#include "os0proc.ic"
#endif

#include "ut0mem.h"
#include "ut0byte.h"

/* FreeBSD for example has only MAP_ANON, Linux has MAP_ANONYMOUS and
MAP_ANON but MAP_ANON is marked as deprecated */
#if defined(MAP_ANONYMOUS)
#define OS_MAP_ANON	MAP_ANONYMOUS
#elif defined(MAP_ANON)
#define OS_MAP_ANON	MAP_ANON
#endif

UNIV_INTERN ibool os_use_large_pages;
/* Large page size. This may be a boot-time option on some platforms */
UNIV_INTERN ulint os_large_page_size;

/********************************************************************
Converts the current process id to a number. It is not guaranteed that the
number is unique. In Linux returns the 'process number' of the current
thread. That number is the same as one sees in 'top', for example. In Linux
the thread id is not the same as one sees in 'top'. */
UNIV_INTERN
ulint
os_proc_get_number(void)
/*====================*/
{
#ifdef __WIN__
	return((ulint)GetCurrentProcessId());
#else
	return((ulint)getpid());
#endif
}

/********************************************************************
Allocates non-cacheable memory. */
UNIV_INTERN
void*
os_mem_alloc_nocache(
/*=================*/
			/* out: allocated memory */
	ulint	n)	/* in: number of bytes */
{
#ifdef __WIN__
	void*	ptr;

	ptr = VirtualAlloc(NULL, n, MEM_COMMIT,
			   PAGE_READWRITE | PAGE_NOCACHE);
	ut_a(ptr);

	return(ptr);
#else
	return(ut_malloc(n));
#endif
}

/********************************************************************
Allocates large pages memory. */
UNIV_INTERN
void*
os_mem_alloc_large(
/*===============*/
					/* out: allocated memory */
	ulint*	n)			/* in/out: number of bytes */
{
	void*	ptr;
	ulint	size;
#if defined HAVE_LARGE_PAGES && defined UNIV_LINUX
	int shmid;
	struct shmid_ds buf;

	if (!os_use_large_pages || !os_large_page_size) {
		goto skip;
	}

	/* Align block size to os_large_page_size */
	size = ut_2pow_round(*n + os_large_page_size - 1, os_large_page_size);

	shmid = shmget(IPC_PRIVATE, (size_t)size, SHM_HUGETLB | SHM_R | SHM_W);
	if (shmid < 0) {
		fprintf(stderr, "InnoDB: HugeTLB: Warning: Failed to allocate"
			" %lu bytes. errno %d\n", size, errno);
		ptr = NULL;
	} else {
		ptr = shmat(shmid, NULL, 0);
		if (ptr == (void *)-1) {
			fprintf(stderr, "InnoDB: HugeTLB: Warning: Failed to"
				" attach shared memory segment, errno %d\n",
				errno);
		}

		/* Remove the shared memory segment so that it will be
		automatically freed after memory is detached or
		process exits */
		shmctl(shmid, IPC_RMID, &buf);
	}

	if (ptr) {
		*n = size;
		ut_total_allocated_memory += size;
# ifdef UNIV_SET_MEM_TO_ZERO
		memset(ptr, '\0', size);
# endif
		return(ptr);
	}

	fprintf(stderr, "InnoDB HugeTLB: Warning: Using conventional"
		" memory pool\n");
skip:
#endif /* HAVE_LARGE_PAGES && UNIV_LINUX */

#ifdef __WIN__
	SYSTEM_INFO	system_info;
	GetSystemInfo(&system_info);

	/* Align block size to system page size */
	size = *n = ut_2pow_round(*n + system_info.dwPageSize - 1,
				  system_info.dwPageSize);
	ptr = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
			   PAGE_READWRITE | PAGE_WRITECOMBINE);
	if (!ptr) {
		fprintf(stderr, "InnoDB: VirtualAlloc(%lu bytes) failed;"
			" Windows error %lu\n",
			(ulong) size, (ulong) GetLastError());
	} else {
		ut_total_allocated_memory += size;
	}
#elif defined __NETWARE__ || !defined OS_MAP_ANON
	size = *n;
	ptr = ut_malloc_low(size, TRUE, FALSE);
#else
# ifdef HAVE_GETPAGESIZE
	size = getpagesize();
# else
	size = UNIV_PAGE_SIZE;
# endif
	/* Align block size to system page size */
	size = *n = ut_2pow_round(*n + size - 1, size);
	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | OS_MAP_ANON, -1, 0);
	if (UNIV_UNLIKELY(ptr == (void*) -1)) {
		fprintf(stderr, "InnoDB: mmap(%lu bytes) failed;"
			" errno %lu\n",
			(ulong) size, (ulong) errno);
		ptr = NULL;
	} else {
		ut_total_allocated_memory += size;
	}
#endif
	return(ptr);
}

/********************************************************************
Frees large pages memory. */
UNIV_INTERN
void
os_mem_free_large(
/*==============*/
	void	*ptr,			/* in: pointer returned by
					os_mem_alloc_large() */
	ulint	size)			/* in: size returned by
					os_mem_alloc_large() */
{
	ut_a(ut_total_allocated_memory >= size);

#if defined HAVE_LARGE_PAGES && defined UNIV_LINUX
	if (os_use_large_pages && os_large_page_size && !shmdt(ptr)) {
		ut_total_allocated_memory -= size;
		return;
	}
#endif /* HAVE_LARGE_PAGES && UNIV_LINUX */
#ifdef __WIN__
	if (!VirtualFree(ptr, size, MEM_DECOMMIT | MEM_RELEASE)) {
		fprintf(stderr, "InnoDB: VirtualFree(%p, %lu) failed;"
			" Windows error %lu\n",
			ptr, (ulong) size, (ulong) GetLastError());
	} else {
		ut_total_allocated_memory -= size;
	}
#elif defined __NETWARE__ || !defined OS_MAP_ANON
	ut_free(ptr);
#else
	if (munmap(ptr, size)) {
		fprintf(stderr, "InnoDB: munmap(%p, %lu) failed;"
			" errno %lu\n",
			ptr, (ulong) size, (ulong) errno);
	} else {
		ut_total_allocated_memory -= size;
	}
#endif
}

/********************************************************************
Sets the priority boost for threads released from waiting within the current
process. */
UNIV_INTERN
void
os_process_set_priority_boost(
/*==========================*/
	ibool	do_boost)	/* in: TRUE if priority boost should be done,
				FALSE if not */
{
#ifdef __WIN__
	ibool	no_boost;

	if (do_boost) {
		no_boost = FALSE;
	} else {
		no_boost = TRUE;
	}

#if TRUE != 1
# error "TRUE != 1"
#endif

	/* Does not do anything currently!
	SetProcessPriorityBoost(GetCurrentProcess(), no_boost);
	*/
	fputs("Warning: process priority boost setting"
	      " currently not functional!\n",
	      stderr);
#else
	UT_NOT_USED(do_boost);
#endif
}
