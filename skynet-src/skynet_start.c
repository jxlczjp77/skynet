#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"
#ifdef _MSC_VER
#include "array.h"
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif // _MSC_VER
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

struct monitor {
	int count;
	struct skynet_monitor ** m;
#ifdef _MSC_VER
	CONDITION_VARIABLE cond;
	CRITICAL_SECTION mutex;
#else
	pthread_cond_t cond;
	pthread_mutex_t mutex;
#endif // _MSC_VER
	int sleep;
	int quit;
};

struct worker_parm {
	struct monitor *m;
	int id;
	int weight;
};

static int SIG = 0;

static void
handle_hup(int signal) {
#ifdef _MSC_VER
	if (signal == SIGBREAK) {
#else
	if (signal == SIGHUP) {
#endif // _MSC_VER
		SIG = 1;
	}
}

#define CHECK_ABORT if (skynet_context_total()==0) break;

#ifdef _MSC_VER
static void
create_thread(HANDLE *thread, LPTHREAD_START_ROUTINE start_routine, void *arg) {
	HANDLE h = CreateThread(NULL, 0, start_routine, arg, 0, NULL);
	if (h == NULL) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
	*thread = h;
}
#else
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}
#endif // _MSC_VER

static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
#ifdef _MSC_VER
		WakeConditionVariable(&m->cond);
#else
		pthread_cond_signal(&m->cond);
#endif // _MSC_VER
	}
}

#ifdef _MSC_VER
static DWORD WINAPI
thread_socket(LPVOID *p) {
#else
static void *
thread_socket(void *p) {
#endif // _MSC_VER
	struct monitor * m = (struct monitor *)p;
	skynet_initthread(THREAD_SOCKET);
	for (;;) {
		int r = skynet_socket_poll();
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT
			continue;
		}
		wakeup(m,0);
	}
	return 0;
}

static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
#ifdef _MSC_VER
	DeleteCriticalSection(&m->mutex);
#else
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
#endif // _MSC_VER
	skynet_free(m->m);
	skynet_free(m);
}
#ifdef _MSC_VER
static DWORD WINAPI
thread_monitor(LPVOID *p) {
#else
static void *
thread_monitor(void *p) {
#endif // _MSC_VER
	struct monitor * m = (struct monitor *)p;
	int i;
	int n = m->count;
	skynet_initthread(THREAD_MONITOR);
	for (;;) {
		CHECK_ABORT
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		for (i=0;i<5;i++) {
			CHECK_ABORT
#ifdef _MSC_VER
				Sleep(1000);
#else
				sleep(1);
#endif // _MSC_VER
		}
	}

	return 0;
}

static void
signal_hup() {
	// make log file reopen

	struct skynet_message smsg;
	smsg.source = 0;
	smsg.session = 0;
	smsg.data = NULL;
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		skynet_context_push(logger, &smsg);
	}
}
#ifdef _MSC_VER
static DWORD WINAPI
thread_timer(LPVOID *p) {
#else
static void *
thread_timer(void *p) {
#endif // _MSC_VER
	struct monitor * m = (struct monitor *)p;
	skynet_initthread(THREAD_TIMER);
	for (;;) {
		skynet_updatetime();
		skynet_socket_updatetime();
		CHECK_ABORT
		wakeup(m,m->count-1);
#ifdef _MSC_VER
		Sleep(20);
#else
		usleep(2500);
#endif // _MSC_VER
		if (SIG) {
			signal_hup();
			SIG = 0;
		}
	}
	// wakeup socket thread
	skynet_socket_exit();
#ifdef _MSC_VER
	EnterCriticalSection(&m->mutex);
	m->quit = 1;
	LeaveCriticalSection(&m->mutex);
	WakeAllConditionVariable(&m->cond);
#else
	// wakeup all worker thread
	pthread_mutex_lock(&m->mutex);
	m->quit = 1;
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->mutex);
#endif // _MSC_VER
	return 0;
}

#ifdef _MSC_VER
static DWORD WINAPI
thread_worker(LPVOID *p) {
#else
static void *
thread_worker(void *p) {
#endif // _MSC_VER
	struct worker_parm *wp = (struct worker_parm *)p;
	int id = wp->id;
	int weight = wp->weight;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	skynet_initthread(THREAD_WORKER);
	struct message_queue * q = NULL;
	while (!m->quit) {
		q = skynet_context_message_dispatch(sm, q, weight);
		if (q == NULL) {
#ifdef _MSC_VER
			EnterCriticalSection(&m->mutex);
			++m->sleep;
			if (!m->quit)
				SleepConditionVariableCS(&m->cond, &m->mutex, INFINITE);
			--m->sleep;
			LeaveCriticalSection(&m->mutex);
#else
			if (pthread_mutex_lock(&m->mutex) == 0) {
				++ m->sleep;
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
#endif // _MSC_VER
		}
	}
	return 0;
}

static void
start(int thread) {
#ifdef _MSC_VER
	#define max_thread 100
	HANDLE pid[max_thread];
	if (thread > max_thread) {
		fprintf(stderr, "Too many thread");
		exit(0);
	}
#else
	pthread_t pid[thread + 3];
#endif // _MSC_VER

	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread;
	m->sleep = 0;

	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}
#ifdef _MSC_VER
	InitializeCriticalSection(&m->mutex);
	InitializeConditionVariable(&m->cond);
#else
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}
#endif // _MSC_VER

	create_thread(&pid[0], thread_monitor, m);
	create_thread(&pid[1], thread_timer, m);
	create_thread(&pid[2], thread_socket, m);

	static int weight[] = { 
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };
#ifdef _MSC_VER
	struct worker_parm wp[max_thread];
#else
	struct worker_parm wp[thread];
#endif
	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];
		} else {
			wp[i].weight = 0;
		}
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

	for (i=0;i<thread+3;i++) {
#ifdef _MSC_VER
		WaitForSingleObject(pid[i], INFINITE);
#else
		pthread_join(pid[i], NULL);
#endif // _MSC_VER

	}

	free_monitor(m);
}

static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline);
#ifdef _MSC_VER
	struct Array name_, args_;
	char *name = AllocArray(&name_, sz + 1);
	char *args = AllocArray(&args_, sz + 1);
#else
	char name[sz+1];
	char args[sz+1];
#endif
	sscanf(cmdline, "%s %s", name, args);
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);
		exit(1);
	}
#ifdef _MSC_VER
	FreeArray(&name_);
	FreeArray(&args_);
#endif
}

void 
skynet_start(struct skynet_config * config) {
	// register SIGHUP for log file reopen
#ifdef _MSC_VER
	signal(SIGBREAK, handle_hup);
#else
	struct sigaction sa;
	sa.sa_handler = &handle_hup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);

	if (config->daemon) {
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}
#endif // _MSC_VER
	skynet_harbor_init(config->harbor);
	skynet_handle_init(config->harbor);
	skynet_mq_init();
	skynet_module_init(config->module_path);
	skynet_timer_init();
	skynet_socket_init();
	skynet_profile_enable(config->profile);

	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	skynet_handle_namehandle(skynet_context_handle(ctx), "logger");

	bootstrap(ctx, config->bootstrap);

	start(config->thread);

	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit();
	skynet_socket_free();
#ifndef _MSC_VER
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
#endif
}
