/*****************************************************************************\
 * src/srun/signals.c - signal handling for srun
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <mgrondona@llnl.gov>, and
 *             Moe Jette     <jette@llnl.gov>
 *  UCRL-CODE-2002-040.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#if HAVE_PTHREAD
#include <pthread.h>
#endif

#include <signal.h>
#include <string.h>

#include <slurm/slurm_errno.h>

#include "src/common/log.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"

#include "src/srun/job.h"
#include "src/srun/io.h"


/* number of active threads */
static pthread_mutex_t active_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  active_cond  = PTHREAD_COND_INITIALIZER;
static int             active = 0;

typedef enum {DSH_NEW, DSH_ACTIVE, DSH_DONE, DSH_FAILED} state_t;

typedef struct thd {
        pthread_t	thread;			/* thread ID */
        pthread_attr_t	attr;			/* thread attributes */
        state_t		state;      		/* thread state */
} thd_t;

typedef struct task_info {
	slurm_msg_t *req_ptr;
	job_t *job_ptr;
	int host_inx;
} task_info_t;


/* 
 * Static prototypes
 */
static void   _sigterm_handler(int);
static void   _handle_intr(job_t *, time_t *, time_t *); 
static inline bool _job_sig_done(job_t *job);
static void   _sig_thr_setup(sigset_t *set);
static void * _sig_thr(void *);
static void   _p_fwd_signal(slurm_msg_t *, job_t *);
static void * _p_signal_task(void *);


static inline bool 
_sig_thr_done(job_t *job)
{
	bool retval;
	slurm_mutex_lock(&job->state_mutex);
	retval = (job->state >= SRUN_JOB_DONE);
	slurm_mutex_unlock(&job->state_mutex);
	return retval;
}

int 
sig_setup_sigmask(void)
{
	sigset_t sigset;

	/* block most signals in all threads, except sigterm */
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGQUIT);
	sigaddset(&sigset, SIGTSTP);
	sigaddset(&sigset, SIGSTOP);
	sigaddset(&sigset, SIGCONT);

	if (sigprocmask(SIG_BLOCK, &sigset, NULL) != 0) {
		error("sigprocmask: %m");
		return SLURM_ERROR;
	}

	xsignal(SIGTERM, &_sigterm_handler);
	xsignal(SIGHUP,  &_sigterm_handler);	/* just an interupt */

	return SLURM_SUCCESS;
}

int 
sig_thr_create(job_t *job)
{
	int e;
	pthread_attr_t attr;

	pthread_attr_init(&attr);

	if ((e = pthread_create(&job->sigid, &attr, &_sig_thr, job)) != 0)
		slurm_seterrno_ret(e);

	debug("Started signals thread (%d)", job->sigid);

	return SLURM_SUCCESS;
}

void 
fwd_signal(job_t *job, int signo)
{
	int i;
	slurm_msg_t *req_array_ptr;
	kill_tasks_msg_t msg;

	debug("forward signal %d to job", signo);

	/* common to all tasks */
	msg.job_id      = job->jobid;
	msg.job_step_id = job->stepid;
	msg.signal      = (uint32_t) signo;

	req_array_ptr = (slurm_msg_t *) 
			xmalloc(sizeof(slurm_msg_t) * job->nhosts);
	for (i = 0; i < job->nhosts; i++) {
		if (job->host_state[i] != SRUN_HOST_REPLIED) {
			debug2("%s has not yet replied\n", job->host[i]);
			continue;
		}

		if (job_active_tasks_on_host(job, i) == 0)
			continue;

		req_array_ptr[i].msg_type = REQUEST_KILL_TASKS;
		req_array_ptr[i].data = &msg;
		memcpy(&req_array_ptr[i].address, 
		       &job->slurmd_addr[i], sizeof(slurm_addr));
	}

	_p_fwd_signal(req_array_ptr, job);

	debug("All tasks have been signalled");
	xfree(req_array_ptr);
}


static void
_sigterm_handler(int signum)
{
	if (signum == SIGTERM) {
		pthread_exit(0);
	}
}

static void
_handle_intr(job_t *job, time_t *last_intr, time_t *last_intr_sent)
{

	if ((time(NULL) - *last_intr) > 1) {
		info("interrupt (one more within 1 sec to abort)");
		if (mode != MODE_ATTACH)
			report_task_status(job);
		*last_intr = time(NULL);
	} else  { /* second Ctrl-C in half as many seconds */

		/* terminate job */
		if (job->state < SRUN_JOB_FORCETERM) {

			info("sending Ctrl-C to job");
			*last_intr = time(NULL);
			fwd_signal(job, SIGINT);

			if ((time(NULL) - *last_intr_sent) < 1)
				job_force_termination(job);
			else
				*last_intr_sent = time(NULL);
		} else {
			job_force_termination(job);
		}
	}
}

static void
_sig_thr_setup(sigset_t *set)
{
	int rc;

	sigemptyset(set);
	sigaddset(set, SIGINT);
	sigaddset(set, SIGQUIT);
	sigaddset(set, SIGTSTP);
	sigaddset(set, SIGSTOP);
	sigaddset(set, SIGCONT);
	if ((rc = pthread_sigmask(SIG_BLOCK, set, NULL)) != 0)
		error ("pthread_sigmask: %s", slurm_strerror(rc));
}

/* simple signal handling thread */
static void *
_sig_thr(void *arg)
{
	job_t *job = (job_t *)arg;
	sigset_t set;
	time_t last_intr      = 0;
	time_t last_intr_sent = 0;
	int signo;

	while (!_sig_thr_done(job)) {

		_sig_thr_setup(&set);

		sigwait(&set, &signo);
		debug2("recvd signal %d", signo);
		switch (signo) {
		  case SIGINT:
			  _handle_intr(job, &last_intr, &last_intr_sent);
			  break;
		  case SIGSTOP:
			debug3("Ignoring SIGSTOP");
			break;
		  case SIGTSTP:
			debug3("got SIGTSTP");
			break;
		  case SIGCONT:
			debug3("got SIGCONT");
			break;
		  case SIGQUIT:
			info("Quit");
			job_force_termination(job);
			break;
		  default:
			break;
		}
	}

	pthread_exit(0);
}

/* _p_fwd_signal - parallel (multi-threaded) task signaller */
static void _p_fwd_signal(slurm_msg_t *req_array_ptr, job_t *job)
{
	int i;
	task_info_t *task_info_ptr;
	thd_t *thread_ptr;

	thread_ptr = xmalloc (job->nhosts * sizeof (thd_t));
	for (i = 0; i < job->nhosts; i++) {
		if (req_array_ptr[i].msg_type == 0)
			continue;	/* inactive task */

		slurm_mutex_lock(&active_mutex);
		while (active >= opt.max_threads) {
			pthread_cond_wait(&active_cond, &active_mutex);
		}
		active++;
		slurm_mutex_unlock(&active_mutex);

		task_info_ptr = (task_info_t *)xmalloc(sizeof(task_info_t));
		task_info_ptr->req_ptr  = &req_array_ptr[i];
		task_info_ptr->job_ptr  = job;
		task_info_ptr->host_inx = i;

		if (pthread_attr_init (&thread_ptr[i].attr))
			error ("pthread_attr_init error %m");
		if (pthread_attr_setdetachstate (&thread_ptr[i].attr, 
		                                 PTHREAD_CREATE_DETACHED))
			error ("pthread_attr_setdetachstate error %m");
#ifdef PTHREAD_SCOPE_SYSTEM
		if (pthread_attr_setscope (&thread_ptr[i].attr, 
		                           PTHREAD_SCOPE_SYSTEM))
			error ("pthread_attr_setscope error %m");
#endif
		while ( pthread_create (&thread_ptr[i].thread, 
		                        &thread_ptr[i].attr, 
		                        _p_signal_task, 
		                        (void *) task_info_ptr) ) {
			error ("pthread_create error %m");
			/* just run it under this thread */
			_p_signal_task((void *) task_info_ptr);
		}
	}


	slurm_mutex_lock(&active_mutex);
	while (active > 0) {
		pthread_cond_wait(&active_cond, &active_mutex);
	}
	slurm_mutex_unlock(&active_mutex);
	xfree(thread_ptr);
}

/* _p_signal_task - parallelized signal of a specific task */
static void * _p_signal_task(void *args)
{
	task_info_t *info = (task_info_t *)args;
	slurm_msg_t *req  = info->req_ptr;
	job_t       *job  = info->job_ptr;
	int     host_inx  = info->host_inx;
	slurm_msg_t resp;

	debug3("sending signal to host %s", job->host[host_inx]);
	if (slurm_send_recv_node_msg(req, &resp) < 0)  /* Has timeout */
		error("signal %s: %m", job->host[host_inx]);
	else {
		return_code_msg_t *rc = resp.data;
		if (rc->return_code != 0) {
			error("%s: Unable to fwd signal: %s",
			       job->host[host_inx], 
			       slurm_strerror(rc->return_code)); 	       
		}

		if (resp.msg_type == RESPONSE_SLURM_RC)
			slurm_free_return_code_msg(resp.data);
	}

	slurm_mutex_lock(&active_mutex);
	active--;
	pthread_cond_signal(&active_cond);
	slurm_mutex_unlock(&active_mutex);
	xfree(args);
	return NULL;
}


