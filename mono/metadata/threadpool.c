/*
 * threadpool.c: global thread pool
 *
 * Authors:
 *   Dietmar Maurer (dietmar@ximian.com)
 *   Gonzalo Paniagua Javier (gonzalo@ximian.com)
 *
 * Copyright 2001-2003 Ximian, Inc (http://www.ximian.com)
 * Copyright 2004-2010 Novell, Inc (http://www.novell.com)
 */

#include <config.h>
#include <glib.h>

#ifdef MONO_SMALL_CONFIG
#define QUEUE_LENGTH 16 /* Must be 2^N */
#define MAX_POOL_THREADS 128
#else
#define QUEUE_LENGTH 64 /* Must be 2^N */
#define MAX_POOL_THREADS 1024
#endif

#include <mono/metadata/domain-internals.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/threads-types.h>
#include <mono/metadata/threadpool-internals.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/file-io.h>
#include <mono/metadata/monitor.h>
#include <mono/metadata/mono-mlist.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/mono-perfcounters.h>
#include <mono/metadata/socket-io.h>
#include <mono/metadata/mono-wsq.h>
#include <mono/io-layer/io-layer.h>
#include <mono/metadata/gc-internal.h>
#include <mono/utils/mono-time.h>
#include <mono/utils/mono-proclib.h>
#include <mono/utils/mono-semaphore.h>
#include <errno.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/types.h>
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <string.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <mono/utils/mono-poll.h>
#ifdef HAVE_EPOLL
#include <sys/epoll.h>
#endif

#ifndef DISABLE_SOCKETS
#include "mono/io-layer/socket-wrappers.h"
#endif

#include "threadpool.h"

#define THREAD_WANTS_A_BREAK(t) ((t->state & (ThreadState_StopRequested | \
						ThreadState_SuspendRequested)) != 0)

#define SPIN_TRYLOCK(i) (InterlockedCompareExchange (&(i), 1, 0) == 0)
#define SPIN_LOCK(i) do { \
				if (SPIN_TRYLOCK (i)) \
					break; \
			} while (1)

#define SPIN_UNLOCK(i) i = 0

#define EPOLL_DEBUG(...)
#define EPOLL_DEBUG_STMT(...)
#define TP_DEBUG(...)
#define TP_DEBUG_STMT(...)

/*
#define EPOLL_DEBUG(...) g_message(__VA_ARGS__)
#define EPOLL_DEBUG_STMT(...) do { __VA_ARGS__ } while (0)
#define TP_DEBUG(...) g_message(__VA_ARGS__)
#define TP_DEBUG_STMT(...) do { __VA_ARGS__ } while (0)
*/

/* map of CounterSample.cs */
struct _MonoCounterSample {
	gint64 rawValue;
	gint64 baseValue;
	gint64 counterFrequency;
	gint64 systemFrequency;
	gint64 timeStamp;
	gint64 timeStamp100nSec;
	gint64 counterTimeStamp;
	int counterType;
};

/* mono_thread_pool_init called */
static volatile int tp_inited;

typedef struct {
	CRITICAL_SECTION io_lock; /* access to sock_to_state */
	int inited;
	int pipe [2];
	MonoGHashTable *sock_to_state;

	HANDLE new_sem; /* access to newpfd and write side of the pipe */
	mono_pollfd *newpfd;
	gboolean epoll_disabled;
#ifdef HAVE_EPOLL
	int epollfd;
#endif
} SocketIOData;

static SocketIOData socket_io_data;

/* Keep in sync with the System.MonoAsyncCall class which provides GC tracking */
typedef struct {
	MonoObject         object;
	MonoMethodMessage *msg;
	MonoMethod        *cb_method;
	MonoDelegate      *cb_target;
	MonoObject        *state;
	MonoObject        *res;
	MonoArray         *out_args;
} ASyncCall;

typedef struct {
	MonoSemType lock;
	MonoMList *first; /* GC root */
	MonoMList *last;
	MonoMList *unused; /* Up to 20 chunks. GC root */
	gint head;
	gint tail;
	MonoSemType new_job;
	volatile gint waiting; /* threads waiting for a work item */

	/**/
	volatile gint pool_status; /* 0 -> not initialized, 1 -> initialized, 2 -> cleaning up */
	/* min, max, n and busy -> Interlocked */
	volatile gint min_threads;
	volatile gint max_threads;
	volatile gint nthreads;
	volatile gint busy_threads;

	void (*async_invoke) (gpointer data);
	void *pc_nitems; /* Performance counter for total number of items in added */
	void *pc_nthreads; /* Performance counter for total number of active threads */
	/**/
	volatile gint destroy_thread;
	volatile gint ignore_times; /* Used when there's a thread being created or destroyed */
	volatile gint sp_lock; /* spin lock used to protect ignore_times */
	volatile gint64 last_check;
	volatile gint64 time_sum;
	volatile gint n_sum;
	gint64 averages [2];
	/**/
	//TP_DEBUG_ONLY (gint nodes_created);
	//TP_DEBUG_ONLY (gint nodes_reused);
	gboolean is_io;
} ThreadPool;

static ThreadPool async_tp;
static ThreadPool async_io_tp;

static void async_invoke_thread (gpointer data);
static MonoObject *mono_async_invoke (ThreadPool *tp, MonoAsyncResult *ares);
static void threadpool_free_queue (ThreadPool *tp);
static void threadpool_append_job (ThreadPool *tp, MonoObject *ar);
static void threadpool_append_jobs (ThreadPool *tp, MonoObject **jobs, gint njobs);
static void threadpool_init (ThreadPool *tp, int min_threads, int max_threads, void (*async_invoke) (gpointer));
static void threadpool_start_idle_threads (ThreadPool *tp);
static void threadpool_kill_idle_threads (ThreadPool *tp);

static MonoClass *async_call_klass;
static MonoClass *socket_async_call_klass;
static MonoClass *process_async_call_klass;

static GPtrArray *wsqs;
CRITICAL_SECTION wsqs_lock;

/* Hooks */
static MonoThreadPoolFunc tp_start_func;
static MonoThreadPoolFunc tp_finish_func;
static gpointer tp_hooks_user_data;
static MonoThreadPoolItemFunc tp_item_begin_func;
static MonoThreadPoolItemFunc tp_item_end_func;
static gpointer tp_item_user_data;

#define INIT_POLLFD(a, b, c) {(a)->fd = b; (a)->events = c; (a)->revents = 0;}
enum {
	AIO_OP_FIRST,
	AIO_OP_ACCEPT = 0,
	AIO_OP_CONNECT,
	AIO_OP_RECEIVE,
	AIO_OP_RECEIVEFROM,
	AIO_OP_SEND,
	AIO_OP_SENDTO,
	AIO_OP_RECV_JUST_CALLBACK,
	AIO_OP_SEND_JUST_CALLBACK,
	AIO_OP_READPIPE,
	AIO_OP_LAST
};

#ifdef DISABLE_SOCKETS
#define socket_io_cleanup(x)
#else
static void
socket_io_cleanup (SocketIOData *data)
{
	if (data->inited == 0)
		return;

	EnterCriticalSection (&data->io_lock);
	data->inited = 0;
#ifdef HOST_WIN32
	closesocket (data->pipe [0]);
	closesocket (data->pipe [1]);
#else
	close (data->pipe [0]);
	close (data->pipe [1]);
#endif
	data->pipe [0] = -1;
	data->pipe [1] = -1;
	if (data->new_sem)
		CloseHandle (data->new_sem);
	data->new_sem = NULL;
	mono_g_hash_table_destroy (data->sock_to_state);
	data->sock_to_state = NULL;
	g_free (data->newpfd);
	data->newpfd = NULL;
#ifdef HAVE_EPOLL
	if (FALSE == data->epoll_disabled)
		close (data->epollfd);
#endif
	LeaveCriticalSection (&data->io_lock);
}

static int
get_event_from_state (MonoSocketAsyncResult *state)
{
	switch (state->operation) {
	case AIO_OP_ACCEPT:
	case AIO_OP_RECEIVE:
	case AIO_OP_RECV_JUST_CALLBACK:
	case AIO_OP_RECEIVEFROM:
	case AIO_OP_READPIPE:
		return MONO_POLLIN;
	case AIO_OP_SEND:
	case AIO_OP_SEND_JUST_CALLBACK:
	case AIO_OP_SENDTO:
	case AIO_OP_CONNECT:
		return MONO_POLLOUT;
	default: /* Should never happen */
		g_message ("get_event_from_state: unknown value in switch!!!");
		return 0;
	}
}

static int
get_events_from_list (MonoMList *list)
{
	MonoSocketAsyncResult *state;
	int events = 0;

	while (list && (state = (MonoSocketAsyncResult *)mono_mlist_get_data (list))) {
		events |= get_event_from_state (state);
		list = mono_mlist_next (list);
	}

	return events;
}

#define ICALL_RECV(x)	ves_icall_System_Net_Sockets_Socket_Receive_internal (\
				(SOCKET)(gssize)x->handle, x->buffer, x->offset, x->size,\
				 x->socket_flags, &x->error);

#define ICALL_SEND(x)	ves_icall_System_Net_Sockets_Socket_Send_internal (\
				(SOCKET)(gssize)x->handle, x->buffer, x->offset, x->size,\
				 x->socket_flags, &x->error);

#endif /* !DISABLE_SOCKETS */

static void
threadpool_jobs_inc (MonoObject *obj)
{
	if (obj)
		InterlockedIncrement (&obj->vtable->domain->threadpool_jobs);
}

static gboolean
threadpool_jobs_dec (MonoObject *obj)
{
	MonoDomain *domain = obj->vtable->domain;
	int remaining_jobs = InterlockedDecrement (&domain->threadpool_jobs);
	if (remaining_jobs == 0 && domain->cleanup_semaphore) {
		ReleaseSemaphore (domain->cleanup_semaphore, 1, NULL);
		return TRUE;
	}
	return FALSE;
}

static MonoObject *
get_io_event (MonoMList **list, gint event)
{
	MonoObject *state;
	MonoMList *current;
	MonoMList *prev;

	current = *list;
	prev = NULL;
	state = NULL;
	while (current) {
		state = mono_mlist_get_data (current);
		if (get_event_from_state ((MonoSocketAsyncResult *) state) == event)
			break;

		state = NULL;
		prev = current;
		current = mono_mlist_next (current);
	}

	if (current) {
		if (prev) {
			mono_mlist_set_next (prev, mono_mlist_next (current));
		} else {
			*list = mono_mlist_next (*list);
		}
	}

	return state;
}

static MonoMList *
process_io_event (MonoMList *list, int event)
{
	MonoSocketAsyncResult *state;
	MonoMList *oldlist;

	oldlist = list;
	state = NULL;
	while (list) {
		state = (MonoSocketAsyncResult *) mono_mlist_get_data (list);
		if (get_event_from_state (state) == event)
			break;
		
		list = mono_mlist_next (list);
	}

	if (list != NULL) {
		oldlist = mono_mlist_remove_item (oldlist, list);
		EPOLL_DEBUG ("Dispatching event %d on socket %p", event, state->handle);
		threadpool_append_job (&async_io_tp, (MonoObject *) state);
	}

	return oldlist;
}

static int
mark_bad_fds (mono_pollfd *pfds, int nfds)
{
	int i, ret;
	mono_pollfd *pfd;
	int count = 0;

	for (i = 0; i < nfds; i++) {
		pfd = &pfds [i];
		if (pfd->fd == -1)
			continue;

		ret = mono_poll (pfd, 1, 0);
		if (ret == -1 && errno == EBADF) {
			pfd->revents |= MONO_POLLNVAL;
			count++;
		} else if (ret == 1) {
			count++;
		}
	}

	return count;
}

static void
socket_io_poll_main (gpointer p)
{
#if MONO_SMALL_CONFIG
#define INITIAL_POLLFD_SIZE	128
#else
#define INITIAL_POLLFD_SIZE	1024
#endif
#define POLL_ERRORS (MONO_POLLERR | MONO_POLLHUP | MONO_POLLNVAL)
	SocketIOData *data = p;
	mono_pollfd *pfds;
	gint maxfd = 1;
	gint allocated;
	gint i;
	MonoInternalThread *thread;

	thread = mono_thread_internal_current ();

	allocated = INITIAL_POLLFD_SIZE;
	pfds = g_new0 (mono_pollfd, allocated);
	INIT_POLLFD (pfds, data->pipe [0], MONO_POLLIN);
	for (i = 1; i < allocated; i++)
		INIT_POLLFD (&pfds [i], -1, 0);

	while (1) {
		int nsock = 0;
		mono_pollfd *pfd;
		char one [1];
		MonoMList *list;

		do {
			if (nsock == -1) {
				if (THREAD_WANTS_A_BREAK (thread))
					mono_thread_interruption_checkpoint ();
			}

			nsock = mono_poll (pfds, maxfd, -1);
		} while (nsock == -1 && errno == EINTR);

		/* 
		 * Apart from EINTR, we only check EBADF, for the rest:
		 *  EINVAL: mono_poll() 'protects' us from descriptor
		 *      numbers above the limit if using select() by marking
		 *      then as MONO_POLLERR.  If a system poll() is being
		 *      used, the number of descriptor we're passing will not
		 *      be over sysconf(_SC_OPEN_MAX), as the error would have
		 *      happened when opening.
		 *
		 *  EFAULT: we own the memory pointed by pfds.
		 *  ENOMEM: we're doomed anyway
		 *
		 */

		if (nsock == -1 && errno == EBADF) {
			pfds->revents = 0; /* Just in case... */
			nsock = mark_bad_fds (pfds, maxfd);
		}

		if ((pfds->revents & POLL_ERRORS) != 0) {
			/* We're supposed to die now, as the pipe has been closed */
			g_free (pfds);
			socket_io_cleanup (data);
			return;
		}

		/* Got a new socket */
		if ((pfds->revents & MONO_POLLIN) != 0) {
			int nread;

			for (i = 1; i < allocated; i++) {
				pfd = &pfds [i];
				if (pfd->fd == -1 || pfd->fd == data->newpfd->fd)
					break;
			}

			if (i == allocated) {
				mono_pollfd *oldfd;

				oldfd = pfds;
				i = allocated;
				allocated = allocated * 2;
				pfds = g_renew (mono_pollfd, oldfd, allocated);
				g_free (oldfd);
				for (; i < allocated; i++)
					INIT_POLLFD (&pfds [i], -1, 0);
			}
#ifndef HOST_WIN32
			nread = read (data->pipe [0], one, 1);
#else
			nread = recv ((SOCKET) data->pipe [0], one, 1, 0);
#endif
			if (nread <= 0) {
				g_free (pfds);
				return; /* we're closed */
			}

			INIT_POLLFD (&pfds [i], data->newpfd->fd, data->newpfd->events);
			ReleaseSemaphore (data->new_sem, 1, NULL);
			if (i >= maxfd)
				maxfd = i + 1;
			nsock--;
		}

		if (nsock == 0)
			continue;

		EnterCriticalSection (&data->io_lock);
		if (data->inited == 0) {
			g_free (pfds);
			LeaveCriticalSection (&data->io_lock);
			return; /* cleanup called */
		}

		for (i = 1; i < maxfd && nsock > 0; i++) {
			pfd = &pfds [i];
			if (pfd->fd == -1 || pfd->revents == 0)
				continue;

			nsock--;
			list = mono_g_hash_table_lookup (data->sock_to_state, GINT_TO_POINTER (pfd->fd));
			if (list != NULL && (pfd->revents & (MONO_POLLIN | POLL_ERRORS)) != 0) {
				list = process_io_event (list, MONO_POLLIN);
			}

			if (list != NULL && (pfd->revents & (MONO_POLLOUT | POLL_ERRORS)) != 0) {
				list = process_io_event (list, MONO_POLLOUT);
			}

			if (list != NULL) {
				mono_g_hash_table_replace (data->sock_to_state, GINT_TO_POINTER (pfd->fd), list);
				pfd->events = get_events_from_list (list);
			} else {
				mono_g_hash_table_remove (data->sock_to_state, GINT_TO_POINTER (pfd->fd));
				pfd->fd = -1;
				if (i == maxfd - 1)
					maxfd--;
			}
		}
		LeaveCriticalSection (&data->io_lock);
	}
}

#ifdef HAVE_EPOLL
#define EPOLL_ERRORS (EPOLLERR | EPOLLHUP)
#define EPOLL_NEVENTS	128
static void
socket_io_epoll_main (gpointer p)
{
	SocketIOData *data;
	int epollfd;
	MonoInternalThread *thread;
	struct epoll_event *events, *evt;
	int ready = 0, i;
	gpointer async_results [EPOLL_NEVENTS * 2]; // * 2 because each loop can add up to 2 results here
	gint nresults;

	data = p;
	epollfd = data->epollfd;
	thread = mono_thread_internal_current ();
	events = g_new0 (struct epoll_event, EPOLL_NEVENTS);

	while (1) {
		do {
			if (ready == -1) {
				if (THREAD_WANTS_A_BREAK (thread))
					mono_thread_interruption_checkpoint ();
			}
			EPOLL_DEBUG ("epoll_wait init");
			ready = epoll_wait (epollfd, events, EPOLL_NEVENTS, -1);
			EPOLL_DEBUG_STMT(
				int err = errno;
				EPOLL_DEBUG ("epoll_wait end with %d ready sockets (%d %s).", ready, err, (err) ? g_strerror (err) : "");
				errno = err;
			);
		} while (ready == -1 && errno == EINTR);

		if (ready == -1) {
			int err = errno;
			g_free (events);
			if (err != EBADF)
				g_warning ("epoll_wait: %d %s", err, g_strerror (err));

			close (epollfd);
			return;
		}

		EnterCriticalSection (&data->io_lock);
		if (data->inited == 0) {
			EPOLL_DEBUG ("data->inited == 0");
			g_free (events);
			close (epollfd);
			return; /* cleanup called */
		}

		nresults = 0;
		for (i = 0; i < ready; i++) {
			int fd;
			MonoMList *list;
			MonoObject *ares;

			evt = &events [i];
			fd = evt->data.fd;
			list = mono_g_hash_table_lookup (data->sock_to_state, GINT_TO_POINTER (fd));
			EPOLL_DEBUG ("Event %d on %d list length: %d", evt->events, fd, mono_mlist_length (list));
			if (list != NULL && (evt->events & (EPOLLIN | EPOLL_ERRORS)) != 0) {
				ares = get_io_event (&list, MONO_POLLIN);
				if (ares != NULL)
					async_results [nresults++] = ares;
			}

			if (list != NULL && (evt->events & (EPOLLOUT | EPOLL_ERRORS)) != 0) {
				ares = get_io_event (&list, MONO_POLLOUT);
				if (ares != NULL)
					async_results [nresults++] = ares;
			}

			if (list != NULL) {
				mono_g_hash_table_replace (data->sock_to_state, GINT_TO_POINTER (fd), list);
				evt->events = get_events_from_list (list);
				EPOLL_DEBUG ("MOD %d to %d", fd, evt->events);
				if (epoll_ctl (epollfd, EPOLL_CTL_MOD, fd, evt)) {
					if (epoll_ctl (epollfd, EPOLL_CTL_ADD, fd, evt) == -1) {
						EPOLL_DEBUG_STMT (
							int err = errno;
							EPOLL_DEBUG ("epoll_ctl(MOD): %d %s fd: %d events: %d", err, g_strerror (err), fd, evt->events);
							errno = err;
						);
					}
				}
			} else {
				mono_g_hash_table_remove (data->sock_to_state, GINT_TO_POINTER (fd));
				EPOLL_DEBUG ("DEL %d", fd);
				epoll_ctl (epollfd, EPOLL_CTL_DEL, fd, evt);
			}
		}
		LeaveCriticalSection (&data->io_lock);
		threadpool_append_jobs (&async_io_tp, (MonoObject **) async_results, nresults);
		memset (async_results, 0, sizeof (gpointer) * nresults);
	}
}
#undef EPOLL_NEVENTS
#endif

/*
 * select/poll wake up when a socket is closed, but epoll just removes
 * the socket from its internal list without notification.
 */
void
mono_thread_pool_remove_socket (int sock)
{
	MonoMList *list, *next;
	MonoSocketAsyncResult *state;

	if (socket_io_data.inited == FALSE)
		return;

	EnterCriticalSection (&socket_io_data.io_lock);
	list = mono_g_hash_table_lookup (socket_io_data.sock_to_state, GINT_TO_POINTER (sock));
	if (list) {
		mono_g_hash_table_remove (socket_io_data.sock_to_state, GINT_TO_POINTER (sock));
	}
	LeaveCriticalSection (&socket_io_data.io_lock);
	
	while (list) {
		state = (MonoSocketAsyncResult *) mono_mlist_get_data (list);
		if (state->operation == AIO_OP_RECEIVE)
			state->operation = AIO_OP_RECV_JUST_CALLBACK;
		else if (state->operation == AIO_OP_SEND)
			state->operation = AIO_OP_SEND_JUST_CALLBACK;

		next = mono_mlist_remove_item (list, list);
		list = process_io_event (list, MONO_POLLIN);
		if (list)
			process_io_event (list, MONO_POLLOUT);

		list = next;
	}
}

#ifdef HOST_WIN32
static void
connect_hack (gpointer x)
{
	struct sockaddr_in *addr = (struct sockaddr_in *) x;
	int count = 0;

	while (connect ((SOCKET) socket_io_data.pipe [1], (SOCKADDR *) addr, sizeof (struct sockaddr_in))) {
		Sleep (500);
		if (++count > 3) {
			g_warning ("Error initializing async. sockets %d.", WSAGetLastError ());
			g_assert (WSAGetLastError ());
		}
	}
}
#endif

static void
socket_io_init (SocketIOData *data)
{
#ifdef HOST_WIN32
	struct sockaddr_in server;
	struct sockaddr_in client;
	SOCKET srv;
	int len;
#endif
	int inited;

	inited = InterlockedCompareExchange (&data->inited, -1, -1);
	if (inited == 1)
		return;

	EnterCriticalSection (&data->io_lock);
	inited = InterlockedCompareExchange (&data->inited, -1, -1);
	if (inited == 1) {
		LeaveCriticalSection (&data->io_lock);
		return;
	}

#ifdef HAVE_EPOLL
	data->epoll_disabled = (g_getenv ("MONO_DISABLE_AIO") != NULL);
	if (FALSE == data->epoll_disabled) {
		data->epollfd = epoll_create (256);
		data->epoll_disabled = (data->epollfd == -1);
		if (data->epoll_disabled && g_getenv ("MONO_DEBUG"))
			g_message ("epoll_create() failed. Using plain poll().");
	} else {
		data->epollfd = -1;
	}
#else
	data->epoll_disabled = TRUE;
#endif

#ifndef HOST_WIN32
	if (data->epoll_disabled) {
		if (pipe (data->pipe) != 0) {
			int err = errno;
			perror ("mono");
			g_assert (err);
		}
	} else {
		data->pipe [0] = -1;
		data->pipe [1] = -1;
	}
#else
	srv = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	g_assert (srv != INVALID_SOCKET);
	data->pipe [1] = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	g_assert (data->pipe [1] != INVALID_SOCKET);

	server.sin_family = AF_INET;
	server.sin_addr.s_addr = inet_addr ("127.0.0.1");
	server.sin_port = 0;
	if (bind (srv, (SOCKADDR *) &server, sizeof (server))) {
		g_print ("%d\n", WSAGetLastError ());
		g_assert (1 != 0);
	}

	len = sizeof (server);
	getsockname (srv, (SOCKADDR *) &server, &len);
	listen (srv, 1);
	mono_thread_create (mono_get_root_domain (), connect_hack, &server);
	len = sizeof (server);
	data->pipe [0] = accept (srv, (SOCKADDR *) &client, &len);
	g_assert (data->pipe [0] != INVALID_SOCKET);
	closesocket (srv);
#endif
	data->sock_to_state = mono_g_hash_table_new_type (g_direct_hash, g_direct_equal, MONO_HASH_VALUE_GC);

	if (data->epoll_disabled) {
		data->new_sem = CreateSemaphore (NULL, 1, 1, NULL);
		g_assert (data->new_sem != NULL);
	}
	if (data->epoll_disabled) {
		mono_thread_create_internal (mono_get_root_domain (), socket_io_poll_main, data, TRUE);
	}
#ifdef HAVE_EPOLL
	else {
		mono_thread_create_internal (mono_get_root_domain (), socket_io_epoll_main, data, TRUE);
	}
#endif
	InterlockedCompareExchange (&data->inited, 1, 0);
	LeaveCriticalSection (&data->io_lock);
}

static void
socket_io_add_poll (MonoSocketAsyncResult *state)
{
	int events;
	char msg [1];
	MonoMList *list;
	SocketIOData *data = &socket_io_data;
	int w;

#if defined(PLATFORM_MACOSX) || defined(PLATFORM_BSD) || defined(HOST_WIN32) || defined(PLATFORM_SOLARIS)
	/* select() for connect() does not work well on the Mac. Bug #75436. */
	/* Bug #77637 for the BSD 6 case */
	/* Bug #78888 for the Windows case */
	if (state->operation == AIO_OP_CONNECT && state->blocking == TRUE) {
		//FIXME: increment number of threads while this one is waiting?
		threadpool_append_job (&async_io_tp, (MonoObject *) state);
		return;
	}
#endif
	WaitForSingleObject (data->new_sem, INFINITE);
	if (data->newpfd == NULL)
		data->newpfd = g_new0 (mono_pollfd, 1);

	EnterCriticalSection (&data->io_lock);
	/* FIXME: 64 bit issue: handle can be a pointer on windows? */
	list = mono_g_hash_table_lookup (data->sock_to_state, GINT_TO_POINTER (state->handle));
	if (list == NULL) {
		list = mono_mlist_alloc ((MonoObject*)state);
	} else {
		list = mono_mlist_append (list, (MonoObject*)state);
	}

	events = get_events_from_list (list);
	INIT_POLLFD (data->newpfd, GPOINTER_TO_INT (state->handle), events);
	mono_g_hash_table_replace (data->sock_to_state, GINT_TO_POINTER (state->handle), list);
	LeaveCriticalSection (&data->io_lock);
	*msg = (char) state->operation;
#ifndef HOST_WIN32
	w = write (data->pipe [1], msg, 1);
	w = w;
#else
	send ((SOCKET) data->pipe [1], msg, 1, 0);
#endif
}

#ifdef HAVE_EPOLL
static gboolean
socket_io_add_epoll (MonoSocketAsyncResult *state)
{
	MonoMList *list;
	SocketIOData *data = &socket_io_data;
	struct epoll_event event;
	int epoll_op, ievt;
	int fd;

	memset (&event, 0, sizeof (struct epoll_event));
	fd = GPOINTER_TO_INT (state->handle);
	EnterCriticalSection (&data->io_lock);
	list = mono_g_hash_table_lookup (data->sock_to_state, GINT_TO_POINTER (fd));
	if (list == NULL) {
		list = mono_mlist_alloc ((MonoObject*)state);
		epoll_op = EPOLL_CTL_ADD;
	} else {
		list = mono_mlist_append (list, (MonoObject*)state);
		epoll_op = EPOLL_CTL_MOD;
	}

	ievt = get_events_from_list (list);
	if ((ievt & MONO_POLLIN) != 0)
		event.events |= EPOLLIN;
	if ((ievt & MONO_POLLOUT) != 0)
		event.events |= EPOLLOUT;

	mono_g_hash_table_replace (data->sock_to_state, state->handle, list);
	event.data.fd = fd;
	EPOLL_DEBUG ("%s %d with %d", epoll_op == EPOLL_CTL_ADD ? "ADD" : "MOD", fd, event.events);
	if (epoll_ctl (data->epollfd, epoll_op, fd, &event) == -1) {
		int err = errno;
		if (epoll_op == EPOLL_CTL_ADD && err == EEXIST) {
			epoll_op = EPOLL_CTL_MOD;
			if (epoll_ctl (data->epollfd, epoll_op, fd, &event) == -1) {
				g_message ("epoll_ctl(MOD): %d %s", err, g_strerror (err));
			}
		}
	}
	LeaveCriticalSection (&data->io_lock);

	return TRUE;
}
#endif

static void
socket_io_add (MonoAsyncResult *ares, MonoSocketAsyncResult *state)
{
	socket_io_init (&socket_io_data);
	MONO_OBJECT_SETREF (state, ares, ares);
#ifdef HAVE_EPOLL
	if (socket_io_data.epoll_disabled == FALSE) {
		if (socket_io_add_epoll (state))
			return;
	}
#endif
	socket_io_add_poll (state);
}

#ifndef DISABLE_SOCKETS
static gboolean
socket_io_filter (MonoObject *target, MonoObject *state)
{
	gint op;
	MonoSocketAsyncResult *sock_res = (MonoSocketAsyncResult *) state;
	MonoClass *klass;

	if (target == NULL || state == NULL)
		return FALSE;

	if (socket_async_call_klass == NULL) {
		klass = target->vtable->klass;
		/* Check if it's SocketAsyncCall in System.Net.Sockets
		 * FIXME: check the assembly is signed correctly for extra care
		 */
		if (klass->name [0] == 'S' && strcmp (klass->name, "SocketAsyncCall") == 0 
				&& strcmp (mono_image_get_name (klass->image), "System") == 0
				&& klass->nested_in && strcmp (klass->nested_in->name, "Socket") == 0)
			socket_async_call_klass = klass;
	}

	if (process_async_call_klass == NULL) {
		klass = target->vtable->klass;
		/* Check if it's AsyncReadHandler in System.Diagnostics.Process
		 * FIXME: check the assembly is signed correctly for extra care
		 */
		if (klass->name [0] == 'A' && strcmp (klass->name, "AsyncReadHandler") == 0 
				&& strcmp (mono_image_get_name (klass->image), "System") == 0
				&& klass->nested_in && strcmp (klass->nested_in->name, "Process") == 0)
			process_async_call_klass = klass;
	}
	/* return both when socket_async_call_klass has not been seen yet and when
	 * the object is not an instance of the class.
	 */
	if (target->vtable->klass != socket_async_call_klass && target->vtable->klass != process_async_call_klass)
		return FALSE;

	op = sock_res->operation;
	if (op < AIO_OP_FIRST || op >= AIO_OP_LAST)
		return FALSE;

	return TRUE;
}
#endif /* !DISABLE_SOCKETS */

/* Returns the exception thrown when invoking, if any */
static MonoObject *
mono_async_invoke (ThreadPool *tp, MonoAsyncResult *ares)
{
	ASyncCall *ac = (ASyncCall *)ares->object_data;
	MonoObject *res, *exc = NULL;
	MonoArray *out_args = NULL;
	HANDLE wait_event = NULL;

	if (ares->execution_context) {
		/* use captured ExecutionContext (if available) */
		MONO_OBJECT_SETREF (ares, original_context, mono_thread_get_execution_context ());
		mono_thread_set_execution_context (ares->execution_context);
	} else {
		ares->original_context = NULL;
	}

	if (ac == NULL) {
		/* Fast path from ThreadPool.*QueueUserWorkItem */
		void *pa = ares->async_state;
		mono_runtime_delegate_invoke (ares->async_delegate, &pa, &exc);
	} else {
		ac->msg->exc = NULL;
		res = mono_message_invoke (ares->async_delegate, ac->msg, &exc, &out_args);
		MONO_OBJECT_SETREF (ac, res, res);
		MONO_OBJECT_SETREF (ac, msg->exc, exc);
		MONO_OBJECT_SETREF (ac, out_args, out_args);

		mono_monitor_enter ((MonoObject *) ares);
		ares->completed = 1;
		if (ares->handle != NULL)
			wait_event = mono_wait_handle_get_handle ((MonoWaitHandle *) ares->handle);
		mono_monitor_exit ((MonoObject *) ares);
		/* notify listeners */
		if (wait_event != NULL)
			SetEvent (wait_event);

		/* call async callback if cb_method != null*/
		if (ac != NULL && ac->cb_method) {
			MonoObject *exc = NULL;
			void *pa = &ares;
			mono_runtime_invoke (ac->cb_method, ac->cb_target, pa, &exc);
			/* 'exc' will be the previous ac->msg->exc if not NULL and not
			 * catched. If catched, this will be set to NULL and the
			 * exception will not be printed. */
			MONO_OBJECT_SETREF (ac->msg, exc, exc);
		}
	}

	/* restore original thread execution context if flow isn't suppressed, i.e. non null */
	if (ares->original_context) {
		mono_thread_set_execution_context (ares->original_context);
		ares->original_context = NULL;
	}
	return exc;
}

static void
threadpool_start_idle_threads (ThreadPool *tp)
{
	int n;

	do {
		while (1) {
			n = tp->nthreads;
			if (n >= tp->min_threads)
				return;
			if (InterlockedCompareExchange (&tp->nthreads, n + 1, n) == n)
				break;
		}
		mono_perfcounter_update_value (tp->pc_nthreads, TRUE, 1);
		mono_thread_create_internal (mono_get_root_domain (), tp->async_invoke, tp, TRUE);
		SleepEx (100, TRUE);
	} while (1);
}

static void
threadpool_init (ThreadPool *tp, int min_threads, int max_threads, void (*async_invoke) (gpointer))
{
	memset (tp, 0, sizeof (ThreadPool));
	MONO_SEM_INIT (&tp->lock, 1);
	tp->min_threads = min_threads;
	tp->max_threads = max_threads;
	tp->async_invoke = async_invoke;
	MONO_SEM_INIT (&tp->new_job, 0);
}

static void *
init_perf_counter (const char *category, const char *counter)
{
	MonoString *category_str;
	MonoString *counter_str;
	MonoString *machine;
	MonoDomain *root;
	MonoBoolean custom;
	int type;

	if (category == NULL || counter == NULL)
		return NULL;
	root = mono_get_root_domain ();
	category_str = mono_string_new (root, category);
	counter_str = mono_string_new (root, counter);
	machine = mono_string_new (root, ".");
	return mono_perfcounter_get_impl (category_str, counter_str, NULL, machine, &type, &custom);
}

void
mono_thread_pool_init ()
{
	gint threads_per_cpu = 1;
	gint thread_count;
	gint cpu_count = mono_cpu_count ();
	int result;

	if (tp_inited == 2)
		return;

	result = InterlockedCompareExchange (&tp_inited, 1, 0);
	if (result == 1) {
		while (1) {
			SleepEx (1, FALSE);
			if (tp_inited == 2)
				return;
		}
	}

	MONO_GC_REGISTER_ROOT (async_tp.first);
	MONO_GC_REGISTER_ROOT (async_tp.last);
	MONO_GC_REGISTER_ROOT (async_tp.unused);
	MONO_GC_REGISTER_ROOT (async_io_tp.first);
	MONO_GC_REGISTER_ROOT (async_io_tp.unused);
	MONO_GC_REGISTER_ROOT (async_io_tp.last);

	MONO_GC_REGISTER_ROOT (socket_io_data.sock_to_state);
	InitializeCriticalSection (&socket_io_data.io_lock);
	if (g_getenv ("MONO_THREADS_PER_CPU") != NULL) {
		threads_per_cpu = atoi (g_getenv ("MONO_THREADS_PER_CPU"));
		if (threads_per_cpu < 1)
			threads_per_cpu = 1;
	}

	thread_count = MIN (cpu_count * threads_per_cpu, MAX_POOL_THREADS);
	threadpool_init (&async_tp, thread_count, MAX_POOL_THREADS, async_invoke_thread);
	threadpool_init (&async_io_tp, cpu_count * 2, cpu_count * 4, async_invoke_thread);
	async_io_tp.is_io = TRUE;

	async_call_klass = mono_class_from_name (mono_defaults.corlib, "System", "MonoAsyncCall");
	g_assert (async_call_klass);

	InitializeCriticalSection (&wsqs_lock);
	wsqs = g_ptr_array_sized_new (MAX_POOL_THREADS);
	mono_wsq_init ();

	async_tp.pc_nitems = init_perf_counter ("Mono Threadpool", "Work Items Added");
	g_assert (async_tp.pc_nitems);

	async_io_tp.pc_nitems = init_perf_counter ("Mono Threadpool", "IO Work Items Added");
	g_assert (async_io_tp.pc_nitems);

	async_tp.pc_nthreads = init_perf_counter ("Mono Threadpool", "# of Threads");
	g_assert (async_tp.pc_nthreads);

	async_io_tp.pc_nthreads = init_perf_counter ("Mono Threadpool", "# of IO Threads");
	g_assert (async_io_tp.pc_nthreads);
	tp_inited = 2;
}

MonoAsyncResult *
mono_thread_pool_add (MonoObject *target, MonoMethodMessage *msg, MonoDelegate *async_callback,
		      MonoObject *state)
{
	MonoDomain *domain = mono_domain_get ();
	MonoAsyncResult *ares;
	ASyncCall *ac;

	ac = (ASyncCall*)mono_object_new (domain, async_call_klass);
	MONO_OBJECT_SETREF (ac, msg, msg);
	MONO_OBJECT_SETREF (ac, state, state);

	if (async_callback) {
		ac->cb_method = mono_get_delegate_invoke (((MonoObject *)async_callback)->vtable->klass);
		MONO_OBJECT_SETREF (ac, cb_target, async_callback);
	}

	ares = mono_async_result_new (domain, NULL, ac->state, NULL, (MonoObject*)ac);
	MONO_OBJECT_SETREF (ares, async_delegate, target);

#ifndef DISABLE_SOCKETS
	if (socket_io_filter (target, state)) {
		socket_io_add (ares, (MonoSocketAsyncResult *) state);
		return ares;
	}
#endif
	threadpool_append_job (&async_tp, (MonoObject *) ares);
	return ares;
}

MonoObject *
mono_thread_pool_finish (MonoAsyncResult *ares, MonoArray **out_args, MonoObject **exc)
{
	ASyncCall *ac;
	HANDLE wait_event;

	*exc = NULL;
	*out_args = NULL;

	/* check if already finished */
	mono_monitor_enter ((MonoObject *) ares);
	
	if (ares->endinvoke_called) {
		*exc = (MonoObject *) mono_get_exception_invalid_operation (NULL);
		mono_monitor_exit ((MonoObject *) ares);
		return NULL;
	}

	ares->endinvoke_called = 1;
	/* wait until we are really finished */
	if (!ares->completed) {
		if (ares->handle == NULL) {
			wait_event = CreateEvent (NULL, TRUE, FALSE, NULL);
			g_assert(wait_event != 0);
			MONO_OBJECT_SETREF (ares, handle, (MonoObject *) mono_wait_handle_new (mono_object_domain (ares), wait_event));
		} else {
			wait_event = mono_wait_handle_get_handle ((MonoWaitHandle *) ares->handle);
		}
		mono_monitor_exit ((MonoObject *) ares);
		WaitForSingleObjectEx (wait_event, INFINITE, TRUE);
	} else {
		mono_monitor_exit ((MonoObject *) ares);
	}

	ac = (ASyncCall *) ares->object_data;
	g_assert (ac != NULL);
	*exc = ac->msg->exc; /* FIXME: GC add write barrier */
	*out_args = ac->out_args;

	return ac->res;
}

static void
threadpool_kill_idle_threads (ThreadPool *tp)
{
	gint n;

	n = (gint) InterlockedCompareExchange (&tp->max_threads, 0, -1);
	while (n) {
		n--;
		MONO_SEM_POST (&tp->new_job);
	}
}

void
mono_thread_pool_cleanup (void)
{
	if (async_tp.pool_status == 1 && InterlockedCompareExchange (&async_tp.pool_status, 2, 1) == 2)
		return;

	InterlockedExchange (&async_io_tp.pool_status, 2);
	MONO_SEM_WAIT (&async_tp.lock);
	threadpool_free_queue (&async_tp);
	threadpool_kill_idle_threads (&async_tp);
	MONO_SEM_POST (&async_tp.lock);

	socket_io_cleanup (&socket_io_data); /* Empty when DISABLE_SOCKETS is defined */
	MONO_SEM_WAIT (&async_io_tp.lock);
	threadpool_free_queue (&async_io_tp);
	threadpool_kill_idle_threads (&async_io_tp);
	MONO_SEM_POST (&async_io_tp.lock);
	MONO_SEM_DESTROY (&async_io_tp.new_job);

	EnterCriticalSection (&wsqs_lock);
	mono_wsq_cleanup ();
	if (wsqs)
		g_ptr_array_free (wsqs, TRUE);
	wsqs = NULL;
	LeaveCriticalSection (&wsqs_lock);
	MONO_SEM_DESTROY (&async_tp.new_job);
}

/* Caller must enter &tp->lock */
static MonoObject*
dequeue_job_nolock (ThreadPool *tp)
{
	MonoObject *ar;
	MonoArray *array;
	MonoMList *list;

	list = tp->first;
	do {
		if (mono_runtime_is_shutting_down ())
			return NULL;
		if (!list || tp->head == tp->tail)
			return NULL;

		array = (MonoArray *) mono_mlist_get_data (list);
		ar = mono_array_get (array, MonoObject *, tp->head % QUEUE_LENGTH);
		mono_array_set (array, MonoObject *, tp->head % QUEUE_LENGTH, NULL);
		tp->head++;
		if ((tp->head % QUEUE_LENGTH) == 0) {
			list = tp->first;
			tp->first = mono_mlist_next (list);
			if (tp->first == NULL)
				tp->last = NULL;
			if (mono_mlist_length (tp->unused) < 20) {
				/* reuse this chunk */
				tp->unused = mono_mlist_set_next (list, tp->unused);
			}
			tp->head -= QUEUE_LENGTH;
			tp->tail -= QUEUE_LENGTH;
		}
		list = tp->first;
	} while (ar == NULL);
	return ar;
}

/* Call after entering &tp->lock */
static gboolean
threadpool_start_thread (ThreadPool *tp)
{
	gint n;

	while ((n = tp->nthreads) < tp->max_threads) {
		if (InterlockedCompareExchange (&tp->nthreads, n + 1, n) == n) {
			mono_perfcounter_update_value (tp->pc_nthreads, TRUE, 1);
			mono_thread_create_internal (mono_get_root_domain (), tp->async_invoke, tp, TRUE);
			return TRUE;
		}
	}

	return FALSE;
}

static void
pulse_on_new_job (ThreadPool *tp)
{
	if (tp->waiting)
		MONO_SEM_POST (&tp->new_job);
}

void
icall_append_job (MonoObject *ar)
{
	threadpool_append_job (&async_tp, ar);
}

static void
threadpool_append_job (ThreadPool *tp, MonoObject *ar)
{
	threadpool_append_jobs (tp, &ar, 1);
}

static MonoMList *
create_or_reuse_list (ThreadPool *tp)
{
	MonoMList *list;
	MonoArray *array;

	list = NULL;
	if (tp->unused) {
		list = tp->unused;
		tp->unused = mono_mlist_next (list);
		mono_mlist_set_next (list, NULL);
		//TP_DEBUG (tp->nodes_reused++);
	} else {
		array = mono_array_new_cached (mono_get_root_domain (), mono_defaults.object_class, QUEUE_LENGTH);
		list = mono_mlist_alloc ((MonoObject *) array);
		//TP_DEBUG (tp->nodes_created++);
	}
	return list;
}

static void
threadpool_append_jobs (ThreadPool *tp, MonoObject **jobs, gint njobs)
{
	MonoArray *array;
	MonoMList *list;
	MonoObject *ar;
	gint i;
	gboolean lock_taken = FALSE; /* We won't take the lock when the local queue is used */

	if (mono_runtime_is_shutting_down ())
		return;

	if (tp->pool_status == 0 && InterlockedCompareExchange (&tp->pool_status, 1, 0) == 0)
		mono_thread_create_internal (mono_get_root_domain (), threadpool_start_idle_threads, tp, TRUE);

	for (i = 0; i < njobs; i++) {
		ar = jobs [i];
		if (ar == NULL)
			continue; /* Might happen when cleaning domain jobs */
		if (!tp->is_io) {
			MonoAsyncResult *o = (MonoAsyncResult *) ar;
			o->add_time = mono_100ns_ticks ();
		}
		threadpool_jobs_inc (ar); 
		mono_perfcounter_update_value (tp->pc_nitems, TRUE, 1);
		if (!tp->is_io && mono_wsq_local_push (ar))
			continue;

		if (!lock_taken) {
			MONO_SEM_WAIT (&tp->lock);
			lock_taken = TRUE;
		}
		if ((tp->tail % QUEUE_LENGTH) == 0) {
			list = create_or_reuse_list (tp);
			if (tp->last != NULL)
				mono_mlist_set_next (tp->last, list);
			tp->last = list;
			if (tp->first == NULL)
				tp->first = tp->last;
		}

		array = (MonoArray *) mono_mlist_get_data (tp->last);
		mono_array_setref (array, tp->tail % QUEUE_LENGTH, ar);
		tp->tail++;
	}
	if (lock_taken)
		MONO_SEM_POST (&tp->lock);
	for (i = 0; i < MIN(njobs, tp->max_threads); i++)
		pulse_on_new_job (tp);
}

static void
threadpool_clear_queue (ThreadPool *tp, MonoDomain *domain)
{
	MonoMList *current;
	MonoArray *array;
	MonoObject *obj;
	int domain_count;
	int i;

	domain_count = 0;
	MONO_SEM_WAIT (&tp->lock);
	current = tp->first;
	while (current) {
		array = (MonoArray *) mono_mlist_get_data (current);
		for (i = 0; i < QUEUE_LENGTH; i++) {
			obj = mono_array_get (array, MonoObject*, i);
			if (obj != NULL && obj->vtable->domain == domain) {
				domain_count++;
				mono_array_setref (array, i, NULL);
			}
		}
		current = mono_mlist_next (current);
	}

	if (!domain_count) {
		MONO_SEM_POST (&tp->lock);
		return;
	}

	current = tp->first;
	tp->first = NULL;
	tp->last = NULL;
	tp->head = 0;
	tp->tail = 0;
	MONO_SEM_POST (&tp->lock);
	/* Re-add everything but the nullified elements */
	while (current) {
		array = (MonoArray *) mono_mlist_get_data (current);
		threadpool_append_jobs (tp, mono_array_addr (array, MonoObject *, 0), QUEUE_LENGTH);
		memset (mono_array_addr (array, MonoObject *, 0), 0, sizeof (MonoObject *) * QUEUE_LENGTH);
		current = mono_mlist_next (current);
	}
}

/*
 * Clean up the threadpool of all domain jobs.
 * Can only be called as part of the domain unloading process as
 * it will wait for all jobs to be visible to the interruption code. 
 */
gboolean
mono_thread_pool_remove_domain_jobs (MonoDomain *domain, int timeout)
{
	HANDLE sem_handle;
	int result = TRUE;
	guint32 start_time = 0;

	g_assert (domain->state == MONO_APPDOMAIN_UNLOADING);

	threadpool_clear_queue (&async_tp, domain);
	threadpool_clear_queue (&async_io_tp, domain);

	/*
	 * There might be some threads out that could be about to execute stuff from the given domain.
	 * We avoid that by setting up a semaphore to be pulsed by the thread that reaches zero.
	 */
	sem_handle = CreateSemaphore (NULL, 0, 1, NULL);
	
	domain->cleanup_semaphore = sem_handle;
	/*
	 * The memory barrier here is required to have global ordering between assigning to cleanup_semaphone
	 * and reading threadpool_jobs.
	 * Otherwise this thread could read a stale version of threadpool_jobs and wait forever.
	 */
	mono_memory_write_barrier ();

	if (domain->threadpool_jobs && timeout != -1)
		start_time = mono_msec_ticks ();
	while (domain->threadpool_jobs) {
		WaitForSingleObject (sem_handle, timeout);
		if (timeout != -1 && (mono_msec_ticks () - start_time) > timeout) {
			result = FALSE;
			break;
		}
	}

	domain->cleanup_semaphore = NULL;
	CloseHandle (sem_handle);
	return result;
}

static void
threadpool_free_queue (ThreadPool *tp)
{
	tp->head = tp->tail = 0;
	tp->first = NULL;
	tp->unused = NULL;
}

gboolean
mono_thread_pool_is_queue_array (MonoArray *o)
{
	gpointer obj = o;

	// FIXME: need some fix in sgen code.
	// There are roots at: async*tp.unused (MonoMList) and wsqs [n]->queue (MonoArray)
	return obj == async_tp.first || obj == async_io_tp.first;
}

static void
add_wsq (MonoWSQ *wsq)
{
	int i;

	if (wsq == NULL)
		return;

	EnterCriticalSection (&wsqs_lock);
	if (wsqs == NULL) {
		LeaveCriticalSection (&wsqs_lock);
		return;
	}
	for (i = 0; i < wsqs->len; i++) {
		if (g_ptr_array_index (wsqs, i) == NULL) {
			wsqs->pdata [i] = wsq;
			LeaveCriticalSection (&wsqs_lock);
			return;
		}
	}
	g_ptr_array_add (wsqs, wsq);
	LeaveCriticalSection (&wsqs_lock);
}

static void
remove_wsq (MonoWSQ *wsq)
{
	int i;

	if (wsq == NULL)
		return;

	EnterCriticalSection (&wsqs_lock);
	if (wsqs == NULL) {
		LeaveCriticalSection (&wsqs_lock);
		return;
	}
	for (i = 0; i < wsqs->len; i++) {
		if (g_ptr_array_index (wsqs, i) == wsq) {
			wsqs->pdata [i] = NULL;
			LeaveCriticalSection (&wsqs_lock);
			return;
		}
	}
	/* Should not happen */
	LeaveCriticalSection (&wsqs_lock);
}

static void
try_steal (gpointer *data, gboolean retry)
{
	int i;
	int ms;

	if (wsqs == NULL || data == NULL || *data != NULL)
		return;

	ms = 0;
	do {
		if (mono_runtime_is_shutting_down ())
			return;
		for (i = 0; wsqs != NULL && i < wsqs->len; i++) {
			if (mono_runtime_is_shutting_down ()) {
				return;
			}
			mono_wsq_try_steal (wsqs->pdata [i], data, ms);
			if (*data != NULL) {
				return;
			}
		}
		ms += 10;
	} while (retry && ms < 11);
}

static gboolean
dequeue_or_steal (ThreadPool *tp, gpointer *data)
{
	if (mono_runtime_is_shutting_down ())
		return FALSE;
	TP_DEBUG ("Dequeue");
	MONO_SEM_WAIT (&tp->lock);
	*data = dequeue_job_nolock (tp);
	MONO_SEM_POST (&tp->lock);
	if (!tp->is_io && !*data)
		try_steal (data, FALSE);
	return (*data != NULL);
}

static void
process_idle_times (ThreadPool *tp, gint64 t)
{
	gint64 ticks;
	gint64 avg;
	gboolean compute_avg;
	gint new_threads;
	gint64 per1;

	if (tp->ignore_times)
		return;

	compute_avg = FALSE;
	ticks = mono_100ns_ticks ();
	t = ticks - t;
	SPIN_LOCK (tp->sp_lock);
	if (tp->ignore_times) {
		SPIN_UNLOCK (tp->sp_lock);
		return;
	}
	tp->time_sum += t;
	tp->n_sum++;
	if (tp->last_check == 0)
		tp->last_check = ticks;
	else if ((ticks - tp->last_check) > 5000000) {
		tp->ignore_times = 1;
		compute_avg = TRUE;
	}
	SPIN_UNLOCK (tp->sp_lock);

	if (!compute_avg)
		return;

	//printf ("Items: %d Time elapsed: %.3fs\n", tp->n_sum, (ticks - tp->last_check) / 10000.0);
	new_threads = 0;
	avg = tp->time_sum / tp->n_sum;
	if (tp->averages [1] == 0) {
		tp->averages [1] = avg;
	} else {
		per1 = ((100 * (ABS (avg - tp->averages [1]))) / tp->averages [1]);
		if (per1 > 5) {
			if (avg > tp->averages [1]) {
				if (tp->averages [1] < tp->averages [0]) {
					new_threads = -1;
				} else {
					new_threads = 1;
				}
			} else if (avg < tp->averages [1] && tp->averages [1] < tp->averages [0]) {
				new_threads = 1;
			}
		} else {
			int min, n;
			min = tp->min_threads;
			n = tp->nthreads;
			if ((n - min) < min && tp->busy_threads == n)
				new_threads = 1;
		}
		/*
		if (new_threads != 0) {
			printf ("n: %d per1: %lld avg=%lld avg1=%lld avg0=%lld\n", new_threads, per1, avg, tp->averages [1], tp->averages [0]);
		}
		*/
	}

	tp->time_sum = 0;
	tp->n_sum = 0;
	tp->last_check = mono_100ns_ticks ();

	tp->averages [0] = tp->averages [1];
	tp->averages [1] = avg;
	tp->ignore_times = 0;

	if (tp->waiting == 0 && new_threads == 1) {
		threadpool_start_thread (tp);
	} else if (new_threads == -1) {
		if (InterlockedCompareExchange (&tp->destroy_thread, 1, 0) == 0)
			pulse_on_new_job (tp);
	}
}

static gboolean
should_i_die (ThreadPool *tp)
{
	gboolean result = FALSE;
	if (tp->destroy_thread == 1 && InterlockedCompareExchange (&tp->destroy_thread, 0, 1) == 1)
		result = (tp->nthreads > tp->min_threads);
	return result;
}

static void
async_invoke_thread (gpointer data)
{
	MonoDomain *domain;
	MonoInternalThread *thread;
	MonoWSQ *wsq;
	ThreadPool *tp;
	gboolean must_die;
  
	tp = data;
	wsq = NULL;
	if (!tp->is_io) {
		wsq = mono_wsq_create ();
		add_wsq (wsq);
	}

	thread = mono_thread_internal_current ();
	if (tp_start_func)
		tp_start_func (tp_hooks_user_data);
	data = NULL;
	for (;;) {
		MonoAsyncResult *ar;
		gboolean is_io_task;

		is_io_task = FALSE;
		ar = (MonoAsyncResult *) data;
		if (ar) {
#ifndef DISABLE_SOCKETS
			is_io_task = (strcmp (((MonoObject *) data)->vtable->klass->name, "AsyncResult"));
			if (is_io_task) {
				MonoSocketAsyncResult *state = (MonoSocketAsyncResult *) data;
				ar = state->ares;
				switch (state->operation) {
				case AIO_OP_RECEIVE:
					state->total = ICALL_RECV (state);
					break;
				case AIO_OP_SEND:
					state->total = ICALL_SEND (state);
					break;
				}
			}
#endif

			/* worker threads invokes methods in different domains,
			 * so we need to set the right domain here */
			domain = ((MonoObject *)ar)->vtable->domain;
			g_assert (domain);

			if (mono_domain_is_unloading (domain) || mono_runtime_is_shutting_down ()) {
				threadpool_jobs_dec ((MonoObject *)ar);
				data = NULL;
				ar = NULL;
				InterlockedDecrement (&tp->busy_threads);
			} else {
				mono_thread_push_appdomain_ref (domain);
				if (threadpool_jobs_dec ((MonoObject *)ar)) {
					data = NULL;
					ar = NULL;
					mono_thread_pop_appdomain_ref ();
					continue;
				}

				if (mono_domain_set (domain, FALSE)) {
					/* ASyncCall *ac; */

					if (tp_item_begin_func)
						tp_item_begin_func (tp_item_user_data);

					if (!is_io_task)
						process_idle_times (tp, ar->add_time);
					/*FIXME: Do something with the exception returned? */
					mono_async_invoke (tp, ar);
					if (tp_item_end_func)
						tp_item_end_func (tp_item_user_data);
					/*
					ac = (ASyncCall *) ar->object_data;
					if (ac->msg->exc != NULL)
						mono_unhandled_exception (ac->msg->exc);
					*/
					mono_domain_set (mono_get_root_domain (), TRUE);
				}
				mono_thread_pop_appdomain_ref ();
				InterlockedDecrement (&tp->busy_threads);
				/* If the callee changes the background status, set it back to TRUE */
				if (!mono_thread_test_state (thread , ThreadState_Background))
					ves_icall_System_Threading_Thread_SetState (thread, ThreadState_Background);
			}
		}

		ar = NULL;
		data = NULL;
		must_die = should_i_die (tp);
		TP_DEBUG ("Trying to get a job");
		if (!must_die && (tp->is_io || !mono_wsq_local_pop (&data)))
			dequeue_or_steal (tp, &data);
		TP_DEBUG ("Done trying to get a job %p", data);

		while (!must_die && !data) {
			gboolean res;

			TP_DEBUG ("Waiting");
			InterlockedIncrement (&tp->waiting);
			while ((res = MONO_SEM_WAIT (&tp->new_job)) && errno == EINTR) {
				if (mono_runtime_is_shutting_down ())
					break;
				if (THREAD_WANTS_A_BREAK (thread))
					mono_thread_interruption_checkpoint ();
			}
			TP_DEBUG ("Done waiting");
			InterlockedDecrement (&tp->waiting);
			//FIXME: res == 0 on windows when interrupted!
			if (mono_runtime_is_shutting_down ())
				break;
			must_die = should_i_die (tp);
			dequeue_or_steal (tp, &data);
		}

		if (!data) {
			gint nt;
			gboolean down;
			while (1) {
				nt = tp->nthreads;
				down = mono_runtime_is_shutting_down ();
				if (!down && nt <= tp->min_threads)
					break;
				if (down || InterlockedCompareExchange (&tp->nthreads, nt - 1, nt) == nt) {
					mono_perfcounter_update_value (tp->pc_nthreads, TRUE, -1);
					TP_DEBUG ("DIE");
					if (!tp->is_io) {
						remove_wsq (wsq);
						mono_wsq_destroy (wsq);
					}
					if (tp_finish_func)
						tp_finish_func (tp_hooks_user_data);
					return;
				}
			}
		}
		
		InterlockedIncrement (&tp->busy_threads);
	}

	g_assert_not_reached ();
}

void
ves_icall_System_Threading_ThreadPool_GetAvailableThreads (gint *workerThreads, gint *completionPortThreads)
{
	*workerThreads = 1024;//async_tp.max_threads - (gint) InterlockedCompareExchange (&async_tp.busy_threads, 0, -1);
	*completionPortThreads = async_io_tp.max_threads - (gint) InterlockedCompareExchange (&async_io_tp.busy_threads, 0, -1);
}

void
ves_icall_System_Threading_ThreadPool_GetMaxThreads (gint *workerThreads, gint *completionPortThreads)
{
	*workerThreads = (gint) InterlockedCompareExchange (&async_tp.max_threads, 0, -1);
	*completionPortThreads = (gint) InterlockedCompareExchange (&async_io_tp.max_threads, 0, -1);
}

void
ves_icall_System_Threading_ThreadPool_GetMinThreads (gint *workerThreads, gint *completionPortThreads)
{
	*workerThreads = (gint) InterlockedCompareExchange (&async_tp.min_threads, 0, -1);
	*completionPortThreads = (gint) InterlockedCompareExchange (&async_io_tp.min_threads, 0, -1);
}

MonoBoolean
ves_icall_System_Threading_ThreadPool_SetMinThreads (gint workerThreads, gint completionPortThreads)
{
	gint max_threads;
	gint max_io_threads;

	max_threads = InterlockedCompareExchange (&async_tp.max_threads, -1, -1);
	if (workerThreads <= 0 || workerThreads > max_threads)
		return FALSE;

	max_io_threads = InterlockedCompareExchange (&async_io_tp.max_threads, -1, -1);
	if (completionPortThreads <= 0 || completionPortThreads > max_io_threads)
		return FALSE;

	InterlockedExchange (&async_tp.min_threads, workerThreads);
	InterlockedExchange (&async_io_tp.min_threads, completionPortThreads);
	//FIXME: check the number of threads before starting this one
	mono_thread_create_internal (mono_get_root_domain (), threadpool_start_idle_threads, &async_tp, TRUE);
	return TRUE;
}

MonoBoolean
ves_icall_System_Threading_ThreadPool_SetMaxThreads (gint workerThreads, gint completionPortThreads)
{
	gint min_threads;
	gint min_io_threads;
	gint cpu_count;

	cpu_count = mono_cpu_count ();
	min_threads = InterlockedCompareExchange (&async_tp.min_threads, -1, -1);
	if (workerThreads < min_threads || workerThreads < cpu_count)
		return FALSE;

	/* We don't really have the concept of completion ports. Do we care here? */
	min_io_threads = InterlockedCompareExchange (&async_io_tp.min_threads, -1, -1);
	if (completionPortThreads < min_io_threads || completionPortThreads < cpu_count)
		return FALSE;

	InterlockedExchange (&async_tp.max_threads, workerThreads);
	InterlockedExchange (&async_io_tp.max_threads, completionPortThreads);
	return TRUE;
}

/**
 * mono_install_threadpool_thread_hooks
 * @start_func: the function to be called right after a new threadpool thread is created. Can be NULL.
 * @finish_func: the function to be called right before a thredpool thread is exiting. Can be NULL.
 * @user_data: argument passed to @start_func and @finish_func.
 *
 * @start_fun will be called right after a threadpool thread is created and @finish_func right before a threadpool thread exits.
 * The calls will be made from the thread itself.
 */
void
mono_install_threadpool_thread_hooks (MonoThreadPoolFunc start_func, MonoThreadPoolFunc finish_func, gpointer user_data)
{
	tp_start_func = start_func;
	tp_finish_func = finish_func;
	tp_hooks_user_data = user_data;
}

/**
 * mono_install_threadpool_item_hooks
 * @begin_func: the function to be called before a threadpool work item processing starts.
 * @end_func: the function to be called after a threadpool work item is finished.
 * @user_data: argument passed to @begin_func and @end_func.
 *
 * The calls will be made from the thread itself and from the same AppDomain
 * where the work item was executed.
 *
 */
void
mono_install_threadpool_item_hooks (MonoThreadPoolItemFunc begin_func, MonoThreadPoolItemFunc end_func, gpointer user_data)
{
	tp_item_begin_func = begin_func;
	tp_item_end_func = end_func;
	tp_item_user_data = user_data;
}

