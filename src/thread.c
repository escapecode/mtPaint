/*	thread.c
	Copyright (C) 2009 Dmitry Groshev

	This file is part of mtPaint.

	mtPaint is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	mtPaint is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with mtPaint in the file COPYING.
*/

#include "global.h"

#include "mygtk.h"
#include "memory.h"
#include "thread.h"


int maxthreads;

#ifdef U_THREADS

#include <time.h>

#if GTK_MAJOR_VERSION == 1
#ifdef G_THREADS_IMPL_POSIX
#include <pthread.h>
#include <sched.h>
#else
#error "Non-POSIX threads not supported with GTK+1"
#endif
#endif

/* Determine number of CPUs/cores */

static int ncores;

#ifdef WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int numcpus()
{
	SYSTEM_INFO info;

	GetSystemInfo(&info);
	return (info.dwNumberOfProcessors);
}

#else

#include <unistd.h>

static int numcpus()
{
#ifdef _SC_NPROCESSORS_ONLN
	return (sysconf(_SC_NPROCESSORS_ONLN));
#elif defined _SC_NPROC_ONLN
	return (sysconf(_SC_NPROC_ONLN));
#elif defined _SC_CRAY_NCPU
	return (sysconf(_SC_CRAY_NCPU));
#else
	return (1);
#endif
}

#endif

#define THREAD_ALIGN    128
#define THREAD_DEALIGN 4096

/* This function takes *two* variable-length NULL-terminated sequences of
 * pointer/size pairs; the first sequence describes shared buffers, the
 * second, thread-local ones.
 * The memory block it allocates is set up the following way:
 * - threaddata structure with array of tcb pointers;
 * - shared buffers (aligned);
 *  - tcb (thread-aligned) for main thread;
 *  - data block (thread-aligned) for main thread;
 *  - private buffers (aligned) for main thread;
 * - the same for each aux thread in order. */

threaddata *talloc(int flags, int tmax, void *data, int dsize, ...)
{
	va_list args;
	threaddata *res;
	char *tmp, *tm2, *tcb0, *tdata;
	tcb *thread;
	size_t align = 0, talign = THREAD_ALIGN;
	size_t sz, tdsz, sharesz, threadsz;
	int i, nt;


	if (!tmax) /* Number of threads is whatever fits the current image */
	{
		int n, m;

		n = mem_height / 3;
// This can overflow, but mtPaint is unlikely to handle a 8+ Tb image anyway :-)
		m = ((size_t)mem_width * mem_height) / (THREAD_DEALIGN + THREAD_ALIGN);
		tmax = n < m ? n : m;
	}

	if (!(nt = maxthreads)) /* Use as many threads as there are cores */
	{
		if (!ncores) /* Autodetect number of cores */
		{
			ncores = numcpus();
			if (ncores < 1) ncores = 1;
		}
		nt = ncores;
	}

	if (tmax > nt) tmax = nt;
	else if (tmax < 1) tmax = 1;

	if ((flags & MA_ALIGN_MASK) == MA_ALIGN_DOUBLE)
		align = sizeof(double) - 1;
	/* Uncomment if data alignment ever exceeds cache line size */
	/* if (align >= talign) talign = align + 1; */

	/* Calculate space */
	va_start(args, dsize);
	sharesz = 0;
	while (va_arg(args, void *))
	{
		sharesz = (sharesz + align) & ~align;
		sharesz += va_arg(args, int);
	}
	threadsz = (sizeof(tcb) + talign - 1) & ~(talign - 1);
	threadsz += dsize;
	while (va_arg(args, void *))
	{
		threadsz = (threadsz + align) & ~align;
		threadsz += va_arg(args, int);
	}
	threadsz = (threadsz + talign - 1) & ~(talign - 1);
	if (!(threadsz & (THREAD_DEALIGN - 1))) threadsz += talign;
	va_end(args);

	/* Allocate space */
	while (TRUE)
	{
		tdsz = sizeof(threaddata) + sizeof(tcb *) * (tmax - 1);
		sz = align ? align + 1 : 0;
		sz += tdsz + sharesz + talign * 2 + threadsz * tmax;
		tmp = calloc(1, sz);
		if (tmp) break;
		if (!(tmax >>= 1)) return (NULL);
	}

	/* Decide what goes where */
	res = (void *)tmp;
	tmp += tdsz;
	tmp = ALIGNED(tmp, align + 1); // Now points to shared space
	tcb0 = tmp + sharesz;
	tcb0 = ALIGNED(tcb0, talign);
	tm2 = ALIGNED(tcb0, THREAD_DEALIGN);
	if (tcb0 == tm2) tcb0 += talign; // Now points to tcb0

	/* Set up tcbs */
	sz = (sizeof(tcb) + talign - 1) & ~(talign - 1);
	res->count = tmax;
	for (i = 0; i < tmax; i++)
	{
		res->threads[i] = thread = (void *)(tcb0 + threadsz * i);
		thread->index = i;
		thread->count = tmax;
		thread->threads = res->threads;
		thread->data = (void *)((char *)thread + sz);
	}

	/* Set up shared data pointers */
	va_start(args, dsize);
	memcpy(tdata = res->threads[0]->data, data, dsize);
	sz = 0;
	while ((tm2 = va_arg(args, void *)))
	{
		tdsz = va_arg(args, int);
		if (!tdsz && (flags & MA_SKIP_ZEROSIZE)) continue;
		tm2 += tdata - (char *)data;
		*(void **)tm2 = (void *)(tmp + sz);
		sz = (sz + tdsz + align) & ~align;
	}
	for (i = 1; i < tmax; i++)
		memcpy(res->threads[i]->data, tdata, dsize);

	/* Set up private data pointers */
	sz = dsize;
	while ((tmp = va_arg(args, void *)))
	{
		tdsz = va_arg(args, int);
		if (!tdsz && (flags & MA_SKIP_ZEROSIZE)) continue;
		sz = (sz + align) & ~align;
		for (i = 0; i < tmax; i++)
		{
			tdata = res->threads[i]->data;
			tm2 = tmp + (tdata - (char *)data);
			*(void **)tm2 = (void *)(tdata + sz);
		}
		sz += tdsz;
	}
	va_end(args);

	/* Copy the data block of thread 0 back, for convenience */
	memcpy(data, res->threads[0]->data, dsize);

	/* Return the prepared block */
	return (res);
}

int thread_progress(tcb *thread)
{
	tcb **tp = thread->threads;
	int i, j, n = thread->count;

	for (i = j = 0; i < n; i++) j += tp[i]->progress;
	if (!progress_update((float)j / thread->tsteps)) return (FALSE);

	for (i = 0; i < n; i++) tp[i]->stop = TRUE;
	return (TRUE);
}

int threads_running;

void launch_threads(thread_func thread, threaddata *tdata, char *title, int total)
{
	tcb *tp;
	clock_t before, now;
	int i, j, n0, n1 = total, flag = FALSE;
#if GTK_MAJOR_VERSION == 1
	pthread_t tid;
	pthread_attr_t attr;
	int attr_failed;

	attr_failed = pthread_attr_init(&attr) ||
#ifdef PTHREAD_SCOPE_SYSTEM
		pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM) ||
#endif
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#endif

	/* Launch aux threads */
	threads_running = TRUE;
	for (i = tdata->count - 1; i > 0; i--)
	{
		tp = tdata->threads[i];
		/* Reinit thread state */
		tp->stop = FALSE; tp->stopped = FALSE;
		tp->progress = 0;
		/* Allocate work to thread */
		tp->step0 = n0 = (n1 * i) / (i + 1);
		tp->nsteps = n1 - n0;
#if GTK_MAJOR_VERSION == 1
		if (attr_failed || pthread_create(&tid, &attr,
			(void *(*)(void *))thread, tp))
#else 
		if (!g_thread_create((GThreadFunc)thread, tp, FALSE, NULL))
#endif
			tp->stop = TRUE , tp->stopped = TRUE; // Failed to launch
		else n1 = n0 , flag = TRUE; // Success - work is now being done
	}
	threads_running = flag;
#if GTK_MAJOR_VERSION == 1
	pthread_attr_destroy(&attr);
#endif

	/* Put main thread to work */
	tp = tdata->threads[0];
	tp->stop = FALSE; tp->stopped = FALSE;
	tp->progress = 0;
	tp->step0 = 0;
	tp->nsteps = n1;
	tp->tsteps = total;
	if (title) progress_init(title, 1); /* Let init/end be done outside */
	thread(tp);

	/* Wait for aux threads to finish, or user to cancel the job */
	flag = 0;
	while (TRUE)
	{
		for (i = j = 1; i < tdata->count; i++)
			j += tdata->threads[i]->stopped;
		if (j >= tdata->count) break; // All threads finished
		if (tdata->threads[0]->stop) // Cancellation requested
		{
			if (!flag) before = clock();
			else
			{
				now = clock();
				if (now - before >= 5 * CLOCKS_PER_SEC)
				{
				/* Major catastrophe - hung thread(s) */
					flag = 2;
					break;
				}
			}
			flag |= 1;
		}
		thread_progress(tdata->threads[0]);
		/* Let 'em run */
#if GTK_MAJOR_VERSION == 1
		sched_yield();
#else
		g_thread_yield();
#endif
	}
	threads_running = FALSE;
	if (title) progress_end();

/* !!! Even with OS threading, killing a thread is not supported on some systems,
 * and if a thread needs killing, it likely has corrupted some data already - WJ */
	if (flag > 1) alert_box(_("Error"),
		_("Helper thread is not responding. Save your work and exit the program."), NULL);
}

#else /* Multithreading disabled - functions manage one thread only */

threaddata *talloc(int flags, int tmax, void *data, int dsize, ...)
{
	va_list args;
	threaddata *res;
	char *tmp, *tm2, *tcb0, *tdata;
	tcb *thread;
	size_t align = 0, talign = sizeof(double);
	size_t sz, tdsz, sharesz, threadsz;


	if ((flags & MA_ALIGN_MASK) == MA_ALIGN_DOUBLE)
		align = sizeof(double) - 1;
	/* Uncomment if data alignment here exceeds sizeof(double) */
	/* if (align >= talign) talign = align + 1; */

	/* Calculate space */
	va_start(args, dsize);
	sharesz = 0;
	while (va_arg(args, void *))
	{
		sharesz = (sharesz + align) & ~align;
		sharesz += va_arg(args, int);
	}
	threadsz = (sizeof(tcb) + talign - 1) & ~(talign - 1);
	threadsz += dsize;
	while (va_arg(args, void *))
	{
		threadsz = (threadsz + align) & ~align;
		threadsz += va_arg(args, int);
	}
	threadsz = (threadsz + talign - 1) & ~(talign - 1);
	va_end(args);

	/* Allocate space */
	sz = align ? align + 1 : 0;
	sz += sizeof(threaddata) + sharesz + talign + threadsz;
	tmp = calloc(1, sz);
	if (!tmp) return (NULL);

	/* Decide what goes where */
	res = (void *)tmp;
	tmp += sizeof(threaddata);
	tmp = ALIGNED(tmp, align + 1); // Now points to shared space
	tcb0 = tmp + sharesz;
	tcb0 = ALIGNED(tcb0, talign);

	/* Set up one tcb */
	sz = (sizeof(tcb) + talign - 1) & ~(talign - 1);
	res->count = 1;
	res->threads[0] = thread = (void *)tcb0;
	thread->index = 0;
	thread->count = 1;
	thread->threads = res->threads;
	thread->data = (void *)((char *)thread + sz);

	/* Set up shared data pointers */
	va_start(args, dsize);
	memcpy(tdata = res->threads[0]->data, data, dsize);
	sz = 0;
	while ((tm2 = va_arg(args, void *)))
	{
		tdsz = va_arg(args, int);
		if (!tdsz && (flags & MA_SKIP_ZEROSIZE)) continue;
		tm2 += tdata - (char *)data;
		*(void **)tm2 = (void *)(tmp + sz);
		sz = (sz + tdsz + align) & ~align;
	}

	/* Set up private data pointers */
	sz = dsize;
	while ((tm2 = va_arg(args, void *)))
	{
		tdsz = va_arg(args, int);
		if (!tdsz && (flags & MA_SKIP_ZEROSIZE)) continue;
		sz = (sz + align) & ~align;
		tm2 += tdata - (char *)data;
		*(void **)tm2 = (void *)(tdata + sz);
		sz += tdsz;
	}
	va_end(args);

	/* Copy the data block back, for convenience */
	memcpy(data, res->threads[0]->data, dsize);

	/* Return the prepared block */
	return (res);
}

void launch_threads(thread_func thread, threaddata *tdata, char *title, int total)
{
	tcb *tp = tdata->threads[0];

	tp->step0 = 0;
	tp->nsteps = total;
	if (title) progress_init(title, 1); /* Let init/end be done outside */
	thread(tp);
	if (title) progress_end();
}

#endif