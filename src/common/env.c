/*****************************************************************************\
 *  src/common/env.c - add an environment variable to environment vector
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <mgrondona@llnl.gov>, Danny Auble <da@llnl.gov>.
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>		/* MAXPATHLEN */
#include "slurm/slurm.h"
#include "src/common/cpu_frequency.h"
#include "src/common/log.h"
#include "src/common/env.h"
#include "src/common/fd.h"
#include "src/common/node_select.h"
#include "src/common/macros.h"
#include "src/common/proc_args.h"
#include "src/common/read_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_step_layout.h"
#include "src/common/slurmdb_defs.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

/*
 * Define slurm-specific aliases for use by plugins, see slurm_xlator.h
 * for details.
 */
strong_alias(setenvf,			slurm_setenvpf);
strong_alias(unsetenvp,			slurm_unsetenvp);
strong_alias(getenvp,			slurm_getenvp);
strong_alias(env_array_create,		slurm_env_array_create);
strong_alias(env_array_merge,		slurm_env_array_merge);
strong_alias(env_array_copy,		slurm_env_array_copy);
strong_alias(env_array_free,		slurm_env_array_free);
strong_alias(env_array_append,		slurm_env_array_append);
strong_alias(env_array_append_fmt,	slurm_env_array_append_fmt);
strong_alias(env_array_overwrite,	slurm_env_array_overwrite);
strong_alias(env_array_overwrite_fmt,	slurm_env_array_overwrite_fmt);
strong_alias(env_unset_environment,	slurm_env_unset_environment);

#define ENV_BUFSIZE (256 * 1024)
#define MAX_ENV_STRLEN (32 * 4096)	/* Needed for CPU_BIND and MEM_BIND on
					 * SGI systems with huge CPU counts */

static int _setup_particulars(uint32_t cluster_flags,
			       char ***dest,
			       dynamic_plugin_data_t *select_jobinfo)
{
	int rc = SLURM_SUCCESS;
	if (cluster_flags & CLUSTER_FLAG_BG) {
		char *bg_part_id = NULL;
		uint32_t node_cnt = 0;
		select_g_select_jobinfo_get(select_jobinfo,
					    SELECT_JOBDATA_BLOCK_ID,
					    &bg_part_id);
		if (bg_part_id) {
			/* check to see if this is a HTC block or not. */
			if (cluster_flags & CLUSTER_FLAG_BGP) {
				uint16_t conn_type[HIGHEST_DIMENSIONS];
				select_g_select_jobinfo_get(
					select_jobinfo,
					SELECT_JOBDATA_CONN_TYPE,
					&conn_type);
				if (conn_type[0] > SELECT_SMALL) {
					/* SUBMIT_POOL over rides
					   HTC_SUBMIT_POOL */
					setenvf(dest, "SUBMIT_POOL", "%s",
						bg_part_id);
				}
			}
			select_g_select_jobinfo_get(
				select_jobinfo,
				SELECT_JOBDATA_BLOCK_NODE_CNT,
				&node_cnt);
			if (node_cnt)
				setenvf(dest, "SLURM_BLOCK_NUM_NODES",
					"%u", node_cnt);

			setenvf(dest, "MPIRUN_PARTITION", "%s", bg_part_id);
			setenvf(dest, "MPIRUN_NOFREE", "%d", 1);
			setenvf(dest, "MPIRUN_NOALLOCATE", "%d", 1);
			xfree(bg_part_id);
			select_g_select_jobinfo_get(select_jobinfo,
						    SELECT_JOBDATA_IONODES,
						    &bg_part_id);
			if (bg_part_id) {
				setenvf(dest, "SLURM_JOB_SUB_MP", "%s",
					bg_part_id);
				xfree(bg_part_id);
			}
		} else
			rc = SLURM_FAILURE;

		if (rc == SLURM_FAILURE) {
			error("Can't set MPIRUN_PARTITION "
			      "environment variable");
		}
	} else if (cluster_flags & CLUSTER_FLAG_CRAY_A) {
		uint32_t resv_id = 0;

		select_g_select_jobinfo_get(select_jobinfo,
					    SELECT_JOBDATA_RESV_ID,
					    &resv_id);
		if (resv_id) {
			setenvf(dest, "BASIL_RESERVATION_ID", "%u", resv_id);
		} else {
			/* This is not an error for a SLURM job allocation with
			 * no compute nodes and no BASIL reservation */
			verbose("Can't set BASIL_RESERVATION_ID "
			        "environment variable");
		}

	} else if (cluster_flags & CLUSTER_FLAG_AIX) {
		setenvf(dest, "LOADLBATCH", "%s", "yes");
	}

	return rc;
}

/*
 *  Return pointer to `name' entry in environment if found, or
 *   pointer to the last entry (i.e. NULL) if `name' is not
 *   currently set in `env'
 *
 */
static char **
_find_name_in_env(char **env, const char *name)
{
	char **ep;

	ep = env;
	while (*ep != NULL) {
		size_t cnt = 0;

		while ( ((*ep)[cnt] == name[cnt])
		      && ( name[cnt] != '\0')
		      && ((*ep)[cnt] != '\0')    )
			++cnt;

		if (name[cnt] == '\0' && (*ep)[cnt] == '=') {
			break;
		} else
			++ep;
	}

	return (ep);
}

/*
 *  Extend memory allocation for env by 1 entry. Make last entry == NULL.
 *   return pointer to last env entry;
 */
static char **
_extend_env(char ***envp)
{
	char **ep;
	size_t newcnt = (xsize (*envp) / sizeof (char *)) + 1;

	*envp = xrealloc (*envp, newcnt * sizeof (char *));

	(*envp)[newcnt - 1] = NULL;
	ep = &((*envp)[newcnt - 2]);

	/*
	 *  Find last non-NULL entry
	 */
	while (*ep == NULL)
		--ep;

	return (++ep);
}

/* return true if the environment variables should not be set for
 *	srun's --get-user-env option */
static bool _discard_env(char *name, char *value)
{
	if ((strcmp(name, "DISPLAY")     == 0) ||
	    (strcmp(name, "ENVIRONMENT") == 0) ||
	    (strcmp(name, "HOSTNAME")    == 0))
		return true;

	return false;
}

/*
 * Return the number of elements in the environment `env'
 */
int
envcount (char **env)
{
	int envc = 0;
	while (env[envc] != NULL)
		envc++;
	return (envc);
}

/*
 * _setenvfs() (stolen from pdsh)
 *
 * Set a variable in the callers environment.  Args are printf style.
 * XXX Space is allocated on the heap and will never be reclaimed.
 * Example: setenvfs("RMS_RANK=%d", rank);
 */
int
setenvfs(const char *fmt, ...)
{
	va_list ap;
	char *buf, *bufcpy, *loc;
	int rc, size;

	buf = xmalloc(ENV_BUFSIZE);
	va_start(ap, fmt);
	vsnprintf(buf, ENV_BUFSIZE, fmt, ap);
	va_end(ap);

	size = strlen(buf);
	bufcpy = xstrdup(buf);
	xfree(buf);

	if (size >= MAX_ENV_STRLEN) {
		if ((loc = strchr(bufcpy, '=')))
			loc[0] = '\0';
		error("environment variable %s is too long", bufcpy);
		xfree(bufcpy);
		rc = ENOMEM;
	} else {
		rc = putenv(bufcpy);
	}

	return rc;
}

int setenvf(char ***envp, const char *name, const char *fmt, ...)
{
	char *value;
	va_list ap;
	int size, rc;

	value = xmalloc(ENV_BUFSIZE);
	va_start(ap, fmt);
	vsnprintf(value, ENV_BUFSIZE, fmt, ap);
	va_end(ap);

	size = strlen(name) + strlen(value) + 2;
	if (size >= MAX_ENV_STRLEN) {
		error("environment variable %s is too long", name);
		return ENOMEM;
	}

	if (envp && *envp) {
		if (env_array_overwrite(envp, name, value) == 1)
			rc = 0;
		else
			rc = 1;
	} else {
		rc = setenv(name, value, 1);
	}

	xfree(value);
	return rc;
}

/*
 *  Remove environment variable `name' from "environment"
 *   contained in `env'
 *
 *  [ This was taken almost verbatim from glibc's
 *    unsetenv()  code. ]
 */
void unsetenvp(char **env, const char *name)
{
	char **ep;

	if (env == NULL)
		return;

	ep = env;
	while ((ep = _find_name_in_env (ep, name)) && (*ep != NULL)) {
		char **dp = ep;
		xfree (*ep);
		do
			dp[0] = dp[1];
		while (*dp++);

		/*  Continue loop in case `name' appears again. */
		++ep;
	}
	return;
}

char *getenvp(char **env, const char *name)
{
	size_t len = strlen(name);
	char **ep;

	if ((env == NULL) || (env[0] == '\0'))
		return (NULL);

	ep = _find_name_in_env (env, name);

	if (*ep != NULL)
		return (&(*ep)[len+1]);

	return NULL;
}

int setup_env(env_t *env, bool preserve_env)
{
	int rc = SLURM_SUCCESS;
	char *dist = NULL, *lllp_dist = NULL;
	char addrbuf[INET_ADDRSTRLEN];
	uint32_t cluster_flags = slurmdb_setup_cluster_flags();

	if (env == NULL)
		return SLURM_ERROR;

	if (!preserve_env && env->ntasks) {
		if (setenvf(&env->env, "SLURM_NTASKS", "%d", env->ntasks)) {
			error("Unable to set SLURM_NTASKS "
			      "environment variable");
			rc = SLURM_FAILURE;
		}
		if (setenvf(&env->env, "SLURM_NPROCS", "%d", env->ntasks)) {
			error("Unable to set SLURM_NPROCS "
			      "environment variable");
			rc = SLURM_FAILURE;
		}
	}

	if (env->cpus_per_task
	   && setenvf(&env->env, "SLURM_CPUS_PER_TASK", "%d",
		      env->cpus_per_task) ) {
		error("Unable to set SLURM_CPUS_PER_TASK");
		rc = SLURM_FAILURE;
	}

	if (env->ntasks_per_node
	   && setenvf(&env->env, "SLURM_NTASKS_PER_NODE", "%d",
		      env->ntasks_per_node) ) {
		error("Unable to set SLURM_NTASKS_PER_NODE");
		rc = SLURM_FAILURE;
	}

	if (env->ntasks_per_socket
	   && setenvf(&env->env, "SLURM_NTASKS_PER_SOCKET", "%d",
		      env->ntasks_per_socket) ) {
		error("Unable to set SLURM_NTASKS_PER_SOCKET");
		rc = SLURM_FAILURE;
	}

	if (env->ntasks_per_core
	   && setenvf(&env->env, "SLURM_NTASKS_PER_CORE", "%d",
		      env->ntasks_per_core) ) {
		error("Unable to set SLURM_NTASKS_PER_CORE");
		rc = SLURM_FAILURE;
	}

	if (env->cpus_on_node
	   && setenvf(&env->env, "SLURM_CPUS_ON_NODE", "%d",
		      env->cpus_on_node) ) {
		error("Unable to set SLURM_CPUS_ON_NODE");
		rc = SLURM_FAILURE;
	}

	set_distribution(env->distribution, &dist, &lllp_dist);
	if (dist)
		if (setenvf(&env->env, "SLURM_DISTRIBUTION", "%s", dist)) {
			error("Can't set SLURM_DISTRIBUTION env variable");
			rc = SLURM_FAILURE;
		}

	if (env->distribution == SLURM_DIST_PLANE)
		if (setenvf(&env->env, "SLURM_DIST_PLANESIZE", "%u",
			    env->plane_size)) {
			error("Can't set SLURM_DIST_PLANESIZE "
			      "env variable");
			rc = SLURM_FAILURE;
		}

	if (lllp_dist)
		if (setenvf(&env->env, "SLURM_DIST_LLLP", "%s", lllp_dist)) {
			error("Can't set SLURM_DIST_LLLP env variable");
			rc = SLURM_FAILURE;
		}


	if (env->cpu_bind_type) {
		char *str_verbose, *str_bind_type, *str_bind_list;
		char *str_bind;
		int len;

		if (env->batch_flag) {
			unsetenvp(env->env, "SBATCH_CPU_BIND_VERBOSE");
			unsetenvp(env->env, "SBATCH_CPU_BIND_TYPE");
			unsetenvp(env->env, "SBATCH_CPU_BIND_LIST");
			unsetenvp(env->env, "SBATCH_CPU_BIND");
		} else {
			unsetenvp(env->env, "SLURM_CPU_BIND_VERBOSE");
			unsetenvp(env->env, "SLURM_CPU_BIND_TYPE");
			unsetenvp(env->env, "SLURM_CPU_BIND_LIST");
			unsetenvp(env->env, "SLURM_CPU_BIND");
		}

		str_verbose = xstrdup ("");
		if (env->cpu_bind_type & CPU_BIND_VERBOSE) {
			xstrcat(str_verbose, "verbose");
		} else {
			xstrcat(str_verbose, "quiet");
		}
		str_bind_type = xstrdup ("");
		if (env->cpu_bind_type & CPU_BIND_TO_THREADS) {
			xstrcat(str_bind_type, "threads,");
		} else if (env->cpu_bind_type & CPU_BIND_TO_CORES) {
			xstrcat(str_bind_type, "cores,");
		} else if (env->cpu_bind_type & CPU_BIND_TO_SOCKETS) {
			xstrcat(str_bind_type, "sockets,");
		} else if (env->cpu_bind_type & CPU_BIND_TO_LDOMS) {
			xstrcat(str_bind_type, "ldoms,");
		} else if (env->cpu_bind_type & CPU_BIND_TO_BOARDS) {
			xstrcat(str_bind_type, "boards,");
		}
		if (env->cpu_bind_type & CPU_BIND_NONE) {
			xstrcat(str_bind_type, "none");
		} else if (env->cpu_bind_type & CPU_BIND_RANK) {
			xstrcat(str_bind_type, "rank");
		} else if (env->cpu_bind_type & CPU_BIND_MAP) {
			xstrcat(str_bind_type, "map_cpu:");
		} else if (env->cpu_bind_type & CPU_BIND_MASK) {
			xstrcat(str_bind_type, "mask_cpu:");
		} else if (env->cpu_bind_type & CPU_BIND_LDRANK) {
			xstrcat(str_bind_type, "rank_ldom");
		} else if (env->cpu_bind_type & CPU_BIND_LDMAP) {
			xstrcat(str_bind_type, "map_ldom:");
		} else if (env->cpu_bind_type & CPU_BIND_LDMASK) {
			xstrcat(str_bind_type, "mask_ldom:");
		}
		len = strlen(str_bind_type);
		if (len) {		/* remove a possible trailing ',' */
		    	if (str_bind_type[len-1] == ',') {
			    	str_bind_type[len-1] = '\0';
			}
		}
		str_bind_list = xstrdup ("");
		if (env->cpu_bind) {
			xstrcat(str_bind_list, env->cpu_bind);
		}
		str_bind = xstrdup ("");
		xstrcat(str_bind, str_verbose);
		if (str_bind[0] && str_bind_type && str_bind_type[0])
			xstrcatchar(str_bind, ',');
		xstrcat(str_bind, str_bind_type);
		xstrcat(str_bind, str_bind_list);

		if (env->batch_flag) {
			if (setenvf(&env->env, "SBATCH_CPU_BIND_VERBOSE",
				    str_verbose)) {
				error("Unable to set SBATCH_CPU_BIND_VERBOSE");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SBATCH_CPU_BIND_TYPE",
				    str_bind_type)) {
				error("Unable to set SBATCH_CPU_BIND_TYPE");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SBATCH_CPU_BIND_LIST",
				    str_bind_list)) {
				error("Unable to set SBATCH_CPU_BIND_LIST");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SBATCH_CPU_BIND", str_bind)) {
				error("Unable to set SBATCH_CPU_BIND");
				rc = SLURM_FAILURE;
			}
		} else {
			if (setenvf(&env->env, "SLURM_CPU_BIND_VERBOSE",
				    str_verbose)) {
				error("Unable to set SLURM_CPU_BIND_VERBOSE");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SLURM_CPU_BIND_TYPE",
				    str_bind_type)) {
				error("Unable to set SLURM_CPU_BIND_TYPE");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SLURM_CPU_BIND_LIST",
				    str_bind_list)) {
				error("Unable to set SLURM_CPU_BIND_LIST");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SLURM_CPU_BIND", str_bind)) {
				error("Unable to set SLURM_CPU_BIND");
				rc = SLURM_FAILURE;
			}
		}
	}

	if (env->mem_bind_type) {
		char *str_verbose, *str_bind_type, *str_bind_list;
		char *str_bind;

		if (env->batch_flag) {
			unsetenvp(env->env, "SBATCH_MEM_BIND_VERBOSE");
			unsetenvp(env->env, "SBATCH_MEM_BIND_TYPE");
			unsetenvp(env->env, "SBATCH_MEM_BIND_LIST");
			unsetenvp(env->env, "SBATCH_MEM_BIND");
		} else {
			unsetenvp(env->env, "SLURM_MEM_BIND_VERBOSE");
			unsetenvp(env->env, "SLURM_MEM_BIND_TYPE");
			unsetenvp(env->env, "SLURM_MEM_BIND_LIST");
			unsetenvp(env->env, "SLURM_MEM_BIND");
		}

		str_verbose = xstrdup ("");
		if (env->mem_bind_type & MEM_BIND_VERBOSE) {
			xstrcat(str_verbose, "verbose");
		} else {
			xstrcat(str_verbose, "quiet");
		}
		str_bind_type = xstrdup ("");
		if (env->mem_bind_type & MEM_BIND_NONE) {
			xstrcat(str_bind_type, "none");
		} else if (env->mem_bind_type & MEM_BIND_RANK) {
			xstrcat(str_bind_type, "rank");
		} else if (env->mem_bind_type & MEM_BIND_MAP) {
			xstrcat(str_bind_type, "map_mem:");
		} else if (env->mem_bind_type & MEM_BIND_MASK) {
			xstrcat(str_bind_type, "mask_mem:");
		} else if (env->mem_bind_type & MEM_BIND_LOCAL) {
			xstrcat(str_bind_type, "local");
		}
		str_bind_list = xstrdup ("");
		if (env->mem_bind) {
			xstrcat(str_bind_list, env->mem_bind);
		}
		str_bind = xstrdup ("");
		xstrcat(str_bind, str_verbose);
		if (str_bind[0]) {		/* add ',' if str_verbose */
			xstrcatchar(str_bind, ',');
		}
		xstrcat(str_bind, str_bind_type);
		xstrcat(str_bind, str_bind_list);

		if (env->batch_flag) {
			if (setenvf(&env->env, "SBATCH_MEM_BIND_VERBOSE",
				    str_verbose)) {
				error("Unable to set SBATCH_MEM_BIND_VERBOSE");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SBATCH_MEM_BIND_TYPE",
				    str_bind_type)) {
				error("Unable to set SBATCH_MEM_BIND_TYPE");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SBATCH_MEM_BIND_LIST",
				    str_bind_list)) {
				error("Unable to set SBATCH_MEM_BIND_LIST");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SBATCH_MEM_BIND", str_bind)) {
				error("Unable to set SBATCH_MEM_BIND");
				rc = SLURM_FAILURE;
			}
		} else {
			if (setenvf(&env->env, "SLURM_MEM_BIND_VERBOSE",
				    str_verbose)) {
				error("Unable to set SLURM_MEM_BIND_VERBOSE");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SLURM_MEM_BIND_TYPE",
				    str_bind_type)) {
				error("Unable to set SLURM_MEM_BIND_TYPE");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SLURM_MEM_BIND_LIST",
				    str_bind_list)) {
				error("Unable to set SLURM_MEM_BIND_LIST");
				rc = SLURM_FAILURE;
			}
			if (setenvf(&env->env, "SLURM_MEM_BIND", str_bind)) {
				error("Unable to set SLURM_MEM_BIND");
				rc = SLURM_FAILURE;
			}
		}
	}

	if ((env->cpu_freq != NO_VAL) && /* Default value from srun */
	    (env->cpu_freq != 0)) {      /* Default value from slurmstepd
					  * for batch jobs */
		int sts;

		if (env->cpu_freq & CPU_FREQ_RANGE_FLAG) {
			char buf[32];
			cpu_freq_to_string(buf, sizeof(buf), env->cpu_freq);
			sts = setenvf(&env->env, "SLURM_CPU_FREQ_REQ", buf);
		} else {
			sts = setenvf(&env->env, "SLURM_CPU_FREQ_REQ", "%u",
				      env->cpu_freq);
		}
		if (sts) {
			error("Unable to set SLURM_CPU_FREQ_REQ");
			rc = SLURM_FAILURE;
		}
	}

	if (env->overcommit
	    && (setenvf(&env->env, "SLURM_OVERCOMMIT", "1"))) {
		error("Unable to set SLURM_OVERCOMMIT environment variable");
		rc = SLURM_FAILURE;
	}

	if (env->slurmd_debug
	    && setenvf(&env->env, "SLURMD_DEBUG", "%d", env->slurmd_debug)) {
		error("Can't set SLURMD_DEBUG environment variable");
		rc = SLURM_FAILURE;
	}

	if (env->labelio
	   && setenvf(&env->env, "SLURM_LABELIO", "1")) {
		error("Unable to set SLURM_LABELIO environment variable");
		rc = SLURM_FAILURE;
	}

	if (env->select_jobinfo) {
		_setup_particulars(cluster_flags, &env->env,
				   env->select_jobinfo);
	}

	if (env->jobid >= 0) {
		if (setenvf(&env->env, "SLURM_JOB_ID", "%d", env->jobid)) {
			error("Unable to set SLURM_JOB_ID environment");
			rc = SLURM_FAILURE;
		}
		/* and for backwards compatibility... */
		if (setenvf(&env->env, "SLURM_JOBID", "%d", env->jobid)) {
			error("Unable to set SLURM_JOBID environment");
			rc = SLURM_FAILURE;
		}
	}

	if (!(cluster_flags & CLUSTER_FLAG_BG)
	    && !(cluster_flags & CLUSTER_FLAG_CRAYXT)) {
		/* These aren't relavant to a system not using Slurm
		   as the launcher.  Since there isn't a flag for that
		   we check for the flags we do have.
		*/
		if (env->task_pid
		    && setenvf(&env->env, "SLURM_TASK_PID", "%d",
			       (int)env->task_pid)) {
			error("Unable to set SLURM_TASK_PID environment "
			      "variable");
			rc = SLURM_FAILURE;
		}
		if (env->nodeid >= 0
		    && setenvf(&env->env, "SLURM_NODEID", "%d", env->nodeid)) {
			error("Unable to set SLURM_NODEID environment");
			rc = SLURM_FAILURE;
		}

		if (env->procid >= 0
		    && setenvf(&env->env, "SLURM_PROCID", "%d", env->procid)) {
			error("Unable to set SLURM_PROCID environment");
			rc = SLURM_FAILURE;
		}

		if (env->localid >= 0
		    && setenvf(&env->env, "SLURM_LOCALID", "%d",
			       env->localid)) {
			error("Unable to set SLURM_LOCALID environment");
			rc = SLURM_FAILURE;
		}
	}

	if (env->stepid >= 0) {
		if (setenvf(&env->env, "SLURM_STEP_ID", "%d", env->stepid)) {
			error("Unable to set SLURM_STEP_ID environment");
			rc = SLURM_FAILURE;
		}
		/* and for backwards compatibility... */
		if (setenvf(&env->env, "SLURM_STEPID", "%d", env->stepid)) {
			error("Unable to set SLURM_STEPID environment");
			rc = SLURM_FAILURE;
		}
	}

	if (!preserve_env && env->nhosts
	    && setenvf(&env->env, "SLURM_NNODES", "%d", env->nhosts)) {
		error("Unable to set SLURM_NNODES environment var");
		rc = SLURM_FAILURE;
	}

	if (env->nhosts
	    && setenvf(&env->env, "SLURM_JOB_NUM_NODES", "%d", env->nhosts)) {
		error("Unable to set SLURM_JOB_NUM_NODES environment var");
		rc = SLURM_FAILURE;
	}

	if (env->nodelist
	    && setenvf(&env->env, "SLURM_NODELIST", "%s", env->nodelist)) {
		error("Unable to set SLURM_NODELIST environment var.");
		rc = SLURM_FAILURE;
	}

	if (env->partition
	    && setenvf(&env->env, "SLURM_JOB_PARTITION", "%s", env->partition)) {
		error("Unable to set SLURM_JOB_PARTITION environment var.");
		rc = SLURM_FAILURE;
	}

	if (!preserve_env && env->task_count
	    && setenvf (&env->env,
			"SLURM_TASKS_PER_NODE", "%s", env->task_count)) {
		error ("Can't set SLURM_TASKS_PER_NODE env variable");
		rc = SLURM_FAILURE;
	}

	if (env->comm_port
	    && setenvf (&env->env, "SLURM_SRUN_COMM_PORT", "%u",
			env->comm_port)) {
		error ("Can't set SLURM_SRUN_COMM_PORT env variable");
		rc = SLURM_FAILURE;
	}

	if (env->cli) {

		slurm_print_slurm_addr (env->cli, addrbuf, INET_ADDRSTRLEN);

		/*
		 *  XXX: Eventually, need a function for slurm_addrs that
		 *   returns just the IP address (not addr:port)
		 */

		if ((dist = strchr (addrbuf, ':')) != NULL)
			*dist = '\0';
		setenvf (&env->env, "SLURM_LAUNCH_NODE_IPADDR", "%s", addrbuf);
	}

	if (env->sgtids
	   && setenvf(&env->env, "SLURM_GTIDS", "%s", env->sgtids)) {
		error("Unable to set SLURM_GTIDS environment variable");
		rc = SLURM_FAILURE;
	}

	if (cluster_flags & CLUSTER_FLAG_AIX) {
		char res_env[128];
		char *debug_env = (char *)getenv("SLURM_LL_API_DEBUG");
		int  debug_num = 0;

		/* MP_POERESTART_ENV causes a warning message for "poe", but
		 * is needed for "poerestart". Presently we have no means to
		 * determine what command a user will execute. We could
		 * possibly add a "srestart" command which would set
		 * MP_POERESTART_ENV, but that presently seems unnecessary. */
		/* setenvf(&env->env, "MP_POERESTART_ENV", res_env); */
		if (debug_env)
			debug_num = atoi(debug_env);
		snprintf(res_env, sizeof(res_env), "SLURM_LL_API_DEBUG=%d",
			debug_num);

		/* Required for AIX/POE systems indicating pre-allocation */
		setenvf(&env->env, "LOADLBATCH", "yes");
		setenvf(&env->env, "LOADL_ACTIVE", "3.2.0");
	}

	if (env->pty_port
	&&  setenvf(&env->env, "SLURM_PTY_PORT", "%hu", env->pty_port)) {
		error("Can't set SLURM_PTY_PORT env variable");
		rc = SLURM_FAILURE;
	}
	if (env->ws_col
	&&  setenvf(&env->env, "SLURM_PTY_WIN_COL", "%hu", env->ws_col)) {
		error("Can't set SLURM_PTY_WIN_COL env variable");
		rc = SLURM_FAILURE;
	}
	if (env->ws_row
	&&  setenvf(&env->env, "SLURM_PTY_WIN_ROW", "%hu", env->ws_row)) {
		error("Can't set SLURM_PTY_WIN_ROW env variable");
		rc = SLURM_FAILURE;
	}
	if (env->ckpt_dir
	&& setenvf(&env->env, "SLURM_CHECKPOINT_IMAGE_DIR", "%s",
		   env->ckpt_dir)) {
		error("Can't set SLURM_CHECKPOINT_IMAGE_DIR env variable");
		rc = SLURM_FAILURE;
	}

	if (env->restart_cnt &&
	    setenvf(&env->env, "SLURM_RESTART_COUNT", "%u", env->restart_cnt)) {
		error("Can't set SLURM_RESTART_COUNT env variable");
		rc = SLURM_FAILURE;
	}

	if (env->user_name) {
		if (setenvf(&env->env, "SLURM_JOB_UID", "%u",
			    (unsigned int) env->uid)) {
			error("Can't set SLURM_JOB_UID env variable");
			rc = SLURM_FAILURE;
		}
		if (setenvf(&env->env, "SLURM_JOB_USER", "%s", env->user_name)){
			error("Can't set SLURM_JOB_USER env variable");
			rc = SLURM_FAILURE;
		}
	}

	return rc;
}

/**********************************************************************
 * From here on are the new environment variable management functions,
 * used by the "new" commands: salloc, sbatch, and the step launch APIs.
 **********************************************************************/

/*
 * Return a string representation of an array of uint16_t elements.
 * Each value in the array is printed in decimal notation and elements
 * are separated by a comma.  If sequential elements in the array
 * contain the same value, the value is written out just once followed
 * by "(xN)", where "N" is the number of times the value is repeated.
 *
 * Example:
 *   The array "1, 2, 1, 1, 1, 3, 2" becomes the string "1,2,1(x3),3,2"
 *
 * Returns an xmalloc'ed string.  Free with xfree().
 */
static char *_uint16_array_to_str(int array_len, const uint16_t *array)
{
	int i;
	int previous = 0;
	char *sep = ",";  /* seperator */
	char *str = xstrdup("");

	if (array == NULL)
		return str;

	for (i = 0; i < array_len; i++) {
		if ((i+1 < array_len)
		    && (array[i] == array[i+1])) {
				previous++;
				continue;
		}

		if (i == array_len-1) /* last time through loop */
			sep = "";
		if (previous > 0) {
			xstrfmtcat(str, "%u(x%u)%s",
				   array[i], previous+1, sep);
		} else {
			xstrfmtcat(str, "%u%s", array[i], sep);
		}
		previous = 0;
	}

	return str;
}


/*
 * The cpus-per-node representation in SLURM (and perhaps tasks-per-node
 * in the future) is stored in a compressed format comprised of two
 * equal-length arrays, and an integer holding the array length.  In one
 * array an element represents a count (number of cpus, number of tasks,
 * etc.), and the corresponding element in the other array contains the
 * number of times the count is repeated sequentially in the uncompressed
 * something-per-node array.
 *
 * This function returns the string representation of the compressed
 * array.  Free with xfree().
 */
extern char *uint32_compressed_to_str(uint32_t array_len,
				      const uint16_t *array,
				      const uint32_t *array_reps)
{
	int i;
	char *sep = ","; /* seperator */
	char *str = xstrdup("");

	for (i = 0; i < array_len; i++) {
		if (i == array_len-1) /* last time through loop */
			sep = "";
		if (array_reps[i] > 1) {
			xstrfmtcat(str, "%u(x%u)%s",
				   array[i], array_reps[i], sep);
		} else {
			xstrfmtcat(str, "%u%s", array[i], sep);
		}
	}

	return str;
}

/*
 * Set in "dest" the environment variables relevant to a SLURM job
 * allocation, overwriting any environment variables of the same name.
 * If the address pointed to by "dest" is NULL, memory will automatically be
 * xmalloc'ed.  The array is terminated by a NULL pointer, and thus is
 * suitable for use by execle() and other env_array_* functions.
 *
 * Sets the variables:
 *	SLURM_JOB_ID
 *	SLURM_JOB_NUM_NODES
 *	SLURM_JOB_NODELIST
 *	SLURM_JOB_CPUS_PER_NODE
 *	SLURM_NODE_ALIASES
 *	LOADLBATCH (AIX only)
 *	SLURM_BG_NUM_NODES, MPIRUN_PARTITION, MPIRUN_NOFREE, and
 *	MPIRUN_NOALLOCATE (BG only)
 *
 * Sets OBSOLETE variables (needed for MPI, do not remove):
 *	SLURM_JOBID
 *	SLURM_NNODES
 *	SLURM_NODELIST
 *	SLURM_TASKS_PER_NODE
 */
int
env_array_for_job(char ***dest, const resource_allocation_response_msg_t *alloc,
		  const job_desc_msg_t *desc)
{
	char *tmp = NULL;
	char *dist = NULL, *lllp_dist = NULL;
	slurm_step_layout_t *step_layout = NULL;
	uint32_t num_tasks = desc->num_tasks;
	int rc = SLURM_SUCCESS;
	uint32_t node_cnt = alloc->node_cnt;
	uint32_t cluster_flags = slurmdb_setup_cluster_flags();

	_setup_particulars(cluster_flags, dest, alloc->select_jobinfo);

	if (cluster_flags & CLUSTER_FLAG_BG) {
		select_g_select_jobinfo_get(alloc->select_jobinfo,
					    SELECT_JOBDATA_NODE_CNT,
					    &node_cnt);
		if (!node_cnt)
			node_cnt = alloc->node_cnt;

		env_array_overwrite_fmt(dest, "SLURM_BG_NUM_NODES",
					"%u", node_cnt);
	}

	env_array_overwrite_fmt(dest, "SLURM_JOB_ID", "%u", alloc->job_id);
	env_array_overwrite_fmt(dest, "SLURM_JOB_NUM_NODES", "%u", node_cnt);
	env_array_overwrite_fmt(dest, "SLURM_JOB_NODELIST", "%s",
				alloc->node_list);
	env_array_overwrite_fmt(dest, "SLURM_NODE_ALIASES", "%s",
				alloc->alias_list);
	env_array_overwrite_fmt(dest, "SLURM_JOB_PARTITION", "%s",
				alloc->partition);

	set_distribution(desc->task_dist, &dist, &lllp_dist);
	if (dist)
		env_array_overwrite_fmt(dest, "SLURM_DISTRIBUTION", "%s",
					dist);

	if (desc->task_dist == SLURM_DIST_PLANE)
		env_array_overwrite_fmt(dest, "SLURM_DIST_PLANESIZE",
					"%u", desc->plane_size);

	if (lllp_dist)
		env_array_overwrite_fmt(dest, "SLURM_DIST_LLLP", "%s",
					lllp_dist);

	tmp = uint32_compressed_to_str(alloc->num_cpu_groups,
					alloc->cpus_per_node,
					alloc->cpu_count_reps);
	env_array_overwrite_fmt(dest, "SLURM_JOB_CPUS_PER_NODE", "%s", tmp);
	xfree(tmp);

	if (alloc->pn_min_memory & MEM_PER_CPU) {
		uint32_t tmp_mem = alloc->pn_min_memory & (~MEM_PER_CPU);
		env_array_overwrite_fmt(dest, "SLURM_MEM_PER_CPU", "%u",
					tmp_mem);
#ifdef HAVE_ALPS_CRAY
		env_array_overwrite_fmt(dest, "APRUN_DEFAULT_MEMORY", "%u",
					tmp_mem);
#endif
	} else if (alloc->pn_min_memory) {
		uint32_t tmp_mem = alloc->pn_min_memory;
#ifdef HAVE_ALPS_CRAY
		uint32_t i, max_cpus_per_node = 1;
		for (i = 0; i < alloc->num_cpu_groups; i++) {
			if ((i == 0) ||
			    (max_cpus_per_node < alloc->cpus_per_node[i])) {
				max_cpus_per_node = alloc->cpus_per_node[i];
			}
		}
		tmp_mem /= max_cpus_per_node;
		env_array_overwrite_fmt(dest, "APRUN_DEFAULT_MEMORY", "%u",
					tmp_mem);
		env_array_overwrite_fmt(dest, "SLURM_MEM_PER_CPU", "%u",
					tmp_mem);
#else
		env_array_overwrite_fmt(dest, "SLURM_MEM_PER_NODE", "%u",
					tmp_mem);
#endif
	}

	/* OBSOLETE, but needed by MPI, do not remove */
	env_array_overwrite_fmt(dest, "SLURM_JOBID", "%u", alloc->job_id);
	env_array_overwrite_fmt(dest, "SLURM_NNODES", "%u", node_cnt);
	env_array_overwrite_fmt(dest, "SLURM_NODELIST", "%s", alloc->node_list);

	if (num_tasks == NO_VAL) {
		/* If we know how many tasks we are going to do then
		   we set SLURM_TASKS_PER_NODE */
		int i=0;
		/* If no tasks were given we can figure it out here
		 * by totalling up the cpus and then dividing by the
		 * number of cpus per task */

		num_tasks = 0;
		for (i = 0; i < alloc->num_cpu_groups; i++) {
			num_tasks += alloc->cpu_count_reps[i]
				* alloc->cpus_per_node[i];
		}
		if ((int)desc->cpus_per_task > 1
		   && desc->cpus_per_task != (uint16_t)NO_VAL)
			num_tasks /= desc->cpus_per_task;
		//num_tasks = desc->min_cpus;
	}

	if (desc->task_dist == SLURM_DIST_ARBITRARY) {
		tmp = desc->req_nodes;
		env_array_overwrite_fmt(dest, "SLURM_ARBITRARY_NODELIST",
					"%s", tmp);
	} else
		tmp = alloc->node_list;

	if (!(step_layout = slurm_step_layout_create(tmp,
						    alloc->cpus_per_node,
						    alloc->cpu_count_reps,
						    node_cnt,
						    num_tasks,
						    desc->cpus_per_task,
						    desc->task_dist,
						    desc->plane_size)))
		return SLURM_ERROR;

	tmp = _uint16_array_to_str(step_layout->node_cnt,
				   step_layout->tasks);
	slurm_step_layout_destroy(step_layout);
	env_array_overwrite_fmt(dest, "SLURM_TASKS_PER_NODE", "%s", tmp);
	xfree(tmp);
	return rc;
}

/*
 * Set in "dest" the environment variables strings relevant to a SLURM batch
 * job allocation, overwriting any environment variables of the same name.
 * If the address pointed to by "dest" is NULL, memory will automatically be
 * xmalloc'ed.  The array is terminated by a NULL pointer, and thus is
 * suitable for use by execle() and other env_array_* functions.
 *
 * Sets the variables:
 *	SLURM_JOB_ID
 *	SLURM_JOB_NUM_NODES
 *	SLURM_JOB_NODELIST
 *	SLURM_JOB_CPUS_PER_NODE
 *	SLURM_NODE_ALIASES
 *	ENVIRONMENT=BATCH
 *	HOSTNAME
 *	LOADLBATCH (AIX only)
 *
 * Sets OBSOLETE variables (needed for MPI, do not remove):
 *	SLURM_JOBID
 *	SLURM_NNODES
 *	SLURM_NODELIST
 *	SLURM_NTASKS
 *	SLURM_TASKS_PER_NODE
 */
extern int
env_array_for_batch_job(char ***dest, const batch_job_launch_msg_t *batch,
			const char *node_name)
{
	char *tmp = NULL;
	uint32_t num_nodes = 0;
	uint32_t num_cpus = 0;
	int i;
	slurm_step_layout_t *step_layout = NULL;
	uint32_t num_tasks = batch->ntasks;
	uint16_t cpus_per_task;
	uint16_t task_dist;
	uint32_t cluster_flags = slurmdb_setup_cluster_flags();

	_setup_particulars(cluster_flags, dest, batch->select_jobinfo);

	/* There is no explicit node count in the batch structure,
	 * so we need to calculate the node count. */
	for (i = 0; i < batch->num_cpu_groups; i++) {
		num_nodes += batch->cpu_count_reps[i];
		num_cpus += batch->cpu_count_reps[i] * batch->cpus_per_node[i];
	}

	env_array_overwrite_fmt(dest, "SLURM_JOB_ID", "%u", batch->job_id);
	env_array_overwrite_fmt(dest, "SLURM_JOB_NUM_NODES", "%u", num_nodes);
	if (cluster_flags & CLUSTER_FLAG_BG) {
		env_array_overwrite_fmt(dest, "SLURM_BG_NUM_NODES",
					"%u", num_nodes);
	}
	if (batch->array_task_id != NO_VAL) {
		env_array_overwrite_fmt(dest, "SLURM_ARRAY_JOB_ID", "%u",
					batch->array_job_id);
		env_array_overwrite_fmt(dest, "SLURM_ARRAY_TASK_ID", "%u",
					batch->array_task_id);
	}
	env_array_overwrite_fmt(dest, "SLURM_JOB_NODELIST", "%s", batch->nodes);
	env_array_overwrite_fmt(dest, "SLURM_JOB_PARTITION", "%s", batch->partition);
	env_array_overwrite_fmt(dest, "SLURM_NODE_ALIASES", "%s",
				batch->alias_list);

	tmp = uint32_compressed_to_str(batch->num_cpu_groups,
				       batch->cpus_per_node,
				       batch->cpu_count_reps);
	env_array_overwrite_fmt(dest, "SLURM_JOB_CPUS_PER_NODE", "%s", tmp);
	xfree(tmp);

	env_array_overwrite_fmt(dest, "ENVIRONMENT", "BATCH");
	if (node_name)
		env_array_overwrite_fmt(dest, "HOSTNAME", "%s", node_name);

	/* OBSOLETE, but needed by MPI, do not remove */
	env_array_overwrite_fmt(dest, "SLURM_JOBID", "%u", batch->job_id);
	env_array_overwrite_fmt(dest, "SLURM_NNODES", "%u", num_nodes);
	env_array_overwrite_fmt(dest, "SLURM_NODELIST", "%s", batch->nodes);

	if ((batch->cpus_per_task != 0) &&
	    (batch->cpus_per_task != (uint16_t) NO_VAL))
		cpus_per_task = batch->cpus_per_task;
	else
		cpus_per_task = 1;	/* default value */

	if (num_tasks) {
		env_array_overwrite_fmt(dest, "SLURM_NTASKS", "%u",
					num_tasks);
		/* keep around for old scripts */
		env_array_overwrite_fmt(dest, "SLURM_NPROCS", "%u",
					num_tasks);
	} else {
		num_tasks = num_cpus / cpus_per_task;
	}

	if ((tmp = getenvp(*dest, "SLURM_ARBITRARY_NODELIST"))) {
		task_dist = SLURM_DIST_ARBITRARY;
	} else {
		tmp = batch->nodes;
		task_dist = SLURM_DIST_BLOCK;
	}

	if (!(step_layout = slurm_step_layout_create(tmp,
						     batch->cpus_per_node,
						     batch->cpu_count_reps,
						     num_nodes,
						     num_tasks,
						     cpus_per_task,
						     task_dist,
						     (uint16_t)NO_VAL)))
		return SLURM_ERROR;

	tmp = _uint16_array_to_str(step_layout->node_cnt,
				   step_layout->tasks);
	slurm_step_layout_destroy(step_layout);
	env_array_overwrite_fmt(dest, "SLURM_TASKS_PER_NODE", "%s", tmp);
	xfree(tmp);

	if (batch->pn_min_memory & MEM_PER_CPU) {
		uint32_t tmp_mem = batch->pn_min_memory & (~MEM_PER_CPU);
		env_array_overwrite_fmt(dest, "SLURM_MEM_PER_CPU", "%u",
					tmp_mem);
#ifdef HAVE_ALPS_CRAY
		env_array_overwrite_fmt(dest, "CRAY_AUTO_APRUN_OPTIONS",
					"\"-m%u\"", tmp_mem);
#endif
	} else if (batch->pn_min_memory) {
		uint32_t tmp_mem = batch->pn_min_memory;
#ifdef HAVE_ALPS_CRAY
		uint32_t i, max_cpus_per_node = 1;
		for (i = 0; i < batch->num_cpu_groups; i++) {
			if ((i == 0) ||
			    (max_cpus_per_node < batch->cpus_per_node[i])) {
				max_cpus_per_node = batch->cpus_per_node[i];
			}
		}
#endif
		env_array_overwrite_fmt(dest, "SLURM_MEM_PER_NODE", "%u",
					tmp_mem);
#ifdef HAVE_ALPS_CRAY
		tmp_mem /= max_cpus_per_node;
		env_array_overwrite_fmt(dest, "CRAY_AUTO_APRUN_OPTIONS",
					"\"-m%u\"", tmp_mem);
#endif
	}

	return SLURM_SUCCESS;

}

/*
 * Set in "dest" the environment variables relevant to a SLURM job step,
 * overwriting any environment variables of the same name.  If the address
 * pointed to by "dest" is NULL, memory will automatically be xmalloc'ed.
 * The array is terminated by a NULL pointer, and thus is suitable for
 * use by execle() and other env_array_* functions.  If preserve_env is
 * true, the variables SLURM_NNODES, SLURM_NTASKS and SLURM_TASKS_PER_NODE
 * remain unchanged.
 *
 * Sets variables:
 *	SLURM_STEP_ID
 *	SLURM_STEP_NUM_NODES
 *	SLURM_STEP_NUM_TASKS
 *	SLURM_STEP_TASKS_PER_NODE
 *	SLURM_STEP_LAUNCHER_PORT
 *	SLURM_STEP_LAUNCHER_IPADDR
 *	SLURM_STEP_RESV_PORTS
 *      SLURM_STEP_SUB_MP
 *
 * Sets OBSOLETE variables:
 *	SLURM_STEPID
 *      SLURM_NNODES
 *	SLURM_NTASKS
 *	SLURM_NODELIST
 *	SLURM_TASKS_PER_NODE
 *	SLURM_SRUN_COMM_PORT
 *	SLURM_LAUNCH_NODE_IPADDR
 *
 */
void
env_array_for_step(char ***dest,
		   const job_step_create_response_msg_t *step,
		   uint16_t launcher_port,
		   bool preserve_env)
{
	char *tmp, *tpn;
	uint32_t node_cnt = step->step_layout->node_cnt;
	uint32_t cluster_flags = slurmdb_setup_cluster_flags();

	tpn = _uint16_array_to_str(step->step_layout->node_cnt,
				   step->step_layout->tasks);
	env_array_overwrite_fmt(dest, "SLURM_STEP_ID", "%u", step->job_step_id);
	env_array_overwrite_fmt(dest, "SLURM_STEP_NODELIST",
				"%s", step->step_layout->node_list);
	if (cluster_flags & CLUSTER_FLAG_BG) {
		char geo_char[HIGHEST_DIMENSIONS+1];

		select_g_select_jobinfo_get(step->select_jobinfo,
					    SELECT_JOBDATA_NODE_CNT,
					    &node_cnt);
		if (!node_cnt)
			node_cnt = step->step_layout->node_cnt;

		select_g_select_jobinfo_sprint(step->select_jobinfo,
					       geo_char, sizeof(geo_char),
					       SELECT_PRINT_GEOMETRY);
		if (geo_char[0] != '0')
			env_array_overwrite_fmt(dest, "SLURM_STEP_GEO",
						"%s", geo_char);
		select_g_select_jobinfo_sprint(step->select_jobinfo,
					       geo_char, sizeof(geo_char),
					       SELECT_PRINT_START_LOC);
		env_array_overwrite_fmt(dest, "SLURM_STEP_START_LOC",
					"%s", geo_char);
	}

	env_array_overwrite_fmt(dest, "SLURM_STEP_NUM_NODES",
				"%u", node_cnt);
	env_array_overwrite_fmt(dest, "SLURM_STEP_NUM_TASKS",
				"%u", step->step_layout->task_cnt);
	env_array_overwrite_fmt(dest, "SLURM_STEP_TASKS_PER_NODE", "%s", tpn);
	env_array_overwrite_fmt(dest, "SLURM_STEP_LAUNCHER_PORT",
				"%hu", launcher_port);
	if (step->resv_ports) {
		env_array_overwrite_fmt(dest, "SLURM_STEP_RESV_PORTS",
					"%s", step->resv_ports);
	}

	tmp = NULL;
	select_g_select_jobinfo_get(step->select_jobinfo,
				    SELECT_JOBDATA_IONODES,
				    &tmp);
	if (tmp) {
		setenvf(dest, "SLURM_STEP_SUB_MP", "%s", tmp);
		xfree(tmp);
	}

	/* OBSOLETE, but needed by some MPI implementations, do not remove */
	env_array_overwrite_fmt(dest, "SLURM_STEPID", "%u", step->job_step_id);
	if (!preserve_env) {
		env_array_overwrite_fmt(dest, "SLURM_NNODES",
					"%u", node_cnt);
		env_array_overwrite_fmt(dest, "SLURM_NTASKS", "%u",
					step->step_layout->task_cnt);
		/* keep around for old scripts */
		env_array_overwrite_fmt(dest, "SLURM_NPROCS",
					"%u", step->step_layout->task_cnt);
		env_array_overwrite_fmt(dest, "SLURM_TASKS_PER_NODE", "%s",
					tpn);
	}
	env_array_overwrite_fmt(dest, "SLURM_SRUN_COMM_PORT",
				"%hu", launcher_port);

	xfree(tpn);
}

/*
 * Enviroment variables set elsewhere
 * ----------------------------------
 *
 * Set by slurmstepd:
 *	SLURM_STEP_NODEID
 *	SLURM_STEP_PROCID
 *	SLURM_STEP_LOCALID
 *
 * OBSOLETE set by slurmstepd:
 *	SLURM_NODEID
 *	SLURM_PROCID
 *	SLURM_LOCALID
 */

/***********************************************************************
 * Environment variable array support functions
 ***********************************************************************/

/*
 * Return an empty environment variable array (contains a single
 * pointer to NULL).
 */
char **env_array_create(void)
{
	char **env_array;

	env_array = (char **)xmalloc(sizeof(char **));
	env_array[0] = NULL;

	return env_array;
}

static int _env_array_update(char ***array_ptr, const char *name,
			     const char *value, bool over_write)
{
	char **ep = NULL;
	char *str = NULL;

	if (array_ptr == NULL)
		return 0;

	if (*array_ptr == NULL)
		*array_ptr = env_array_create();

	ep = _find_name_in_env(*array_ptr, name);
	if (*ep != NULL) {
		if (!over_write)
			return 0;
		xfree (*ep);
	} else {
		ep = _extend_env(array_ptr);
	}

	xstrfmtcat(str, "%s=%s", name, value);
	*ep = str;

	return 1;
}

/*
 * Append a single environment variable to an environment variable array,
 * if and only if a variable by that name does not already exist in the
 * array.
 *
 * "value_fmt" supports printf-style formatting.
 *
 * Return 1 on success, and 0 on error.
 */
int env_array_append_fmt(char ***array_ptr, const char *name,
			 const char *value_fmt, ...)
{
	int rc;
	char *value;
	va_list ap;

	value = xmalloc(ENV_BUFSIZE);
	va_start(ap, value_fmt);
	vsnprintf (value, ENV_BUFSIZE, value_fmt, ap);
	va_end(ap);
	rc = env_array_append(array_ptr, name, value);
	xfree(value);

	return rc;
}

/*
 * Append a single environment variable to an environment variable array,
 * if and only if a variable by that name does not already exist in the
 * array.
 *
 * Return 1 on success, and 0 on error.
 */
int env_array_append(char ***array_ptr, const char *name,
		     const char *value)
{
	return _env_array_update(array_ptr, name, value, false);
}

/*
 * Append a single environment variable to an environment variable array
 * if a variable by that name does not already exist.  If a variable
 * by the same name is found in the array, it is overwritten with the
 * new value.
 *
 * "value_fmt" supports printf-style formatting.
 *
 * Return 1 on success, and 0 on error.
 */
int env_array_overwrite_fmt(char ***array_ptr, const char *name,
			    const char *value_fmt, ...)
{
	int rc;
	char *value;
	va_list ap;

	value = xmalloc(ENV_BUFSIZE);
	va_start(ap, value_fmt);
	vsnprintf (value, ENV_BUFSIZE, value_fmt, ap);
	va_end(ap);
	rc = env_array_overwrite(array_ptr, name, value);
	xfree(value);

	return rc;
}

/*
 * Append a single environment variable to an environment variable array
 * if a variable by that name does not already exist.  If a variable
 * by the same name is found in the array, it is overwritten with the
 * new value.
 *
 * Return 1 on success, and 0 on error.
 */
int env_array_overwrite(char ***array_ptr, const char *name,
			const char *value)
{
	return _env_array_update(array_ptr, name, value, true);
}

/*
 * Copy env_array must be freed by env_array_free
 */
char **env_array_copy(const char **array)
{
	char **ptr = NULL;

	env_array_merge(&ptr, array);

	return ptr;
}

/*
 * Free the memory used by an environment variable array.
 */
void env_array_free(char **env_array)
{
	char **ptr;

	if (env_array == NULL)
		return;

	for (ptr = env_array; *ptr != NULL; ptr++) {
		xfree(*ptr);
	}
	xfree(env_array);
}

/*
 * Given an environment variable "name=value" string,
 * copy the name portion into the "name" buffer, and the
 * value portion into the "value" buffer.
 *
 * Return 1 on success, 0 on failure.
 */
static int _env_array_entry_splitter(const char *entry,
				     char *name, int name_len,
				     char *value, int value_len)
{
	char *ptr;
	int len;

	ptr = index(entry, '=');
	if (ptr == NULL)	/* Bad parsing, no '=' found */
		return 0;
	len = ptr - entry;
	if (len > name_len-1)
		return 0;
	strncpy(name, entry, len);
	name[len] = '\0';

	ptr = ptr + 1;
	len = strlen(ptr);
	if (len > value_len-1)
		return 0;
	strncpy(value, ptr, len);
	value[len] = '\0';

	return 1;
}

/*
 * Work similarly to putenv() (from C stdlib), but uses setenv()
 * under the covers.  This avoids having pointers from the global
 * array "environ" into "string".
 *
 * Return 1 on success, 0 on failure.
 */
static int _env_array_putenv(const char *string)
{
	int rc = 0;
	char name[256], *value;

	value = xmalloc(ENV_BUFSIZE);
	if ((_env_array_entry_splitter(string, name, sizeof(name),
				       value, ENV_BUFSIZE)) &&
	    (setenv(name, value, 1) != -1))
		rc = 1;

	xfree(value);
	return rc;
}

/*
 * Set all of the environment variables in a supplied environment
 * variable array.
 */
void env_array_set_environment(char **env_array)
{
	char **ptr;

	if (env_array == NULL)
		return;

	for (ptr = env_array; *ptr != NULL; ptr++) {
		_env_array_putenv(*ptr);
	}
}

/*
 * Unset all of the environment variables in a user's current
 * environment.
 *
 * (Note: becuae the environ array is decrementing with each
 *  unsetenv, only increment the ptr on a failure to unset.)
 */
void env_unset_environment(void)
{
	extern char **environ;
	char **ptr;
	char name[256], *value;

	value = xmalloc(ENV_BUFSIZE);
	for (ptr = (char **)environ; *ptr != NULL; ) {
		if ((_env_array_entry_splitter(*ptr, name, sizeof(name),
					       value, ENV_BUFSIZE)) &&
			(unsetenv(name) != -1))
			;
		else
			ptr++;
	}
	xfree(value);
}

/*
 * Merge all of the environment variables in src_array into the
 * array dest_array.  Any variables already found in dest_array
 * will be overwritten with the value from src_array.
 */
void env_array_merge(char ***dest_array, const char **src_array)
{
	char **ptr;
	char name[256], *value;

	if (src_array == NULL)
		return;

	value = xmalloc(ENV_BUFSIZE);
	for (ptr = (char **)src_array; *ptr != NULL; ptr++) {
		if (_env_array_entry_splitter(*ptr, name, sizeof(name),
					      value, ENV_BUFSIZE))
			env_array_overwrite(dest_array, name, value);
	}
	xfree(value);
}

/*
 * Merge the environment variables in src_array beginning with "SLURM" into the
 * array dest_array.  Any variables already found in dest_array will be
 * overwritten with the value from src_array.
 */
void env_array_merge_slurm(char ***dest_array, const char **src_array)
{
	char **ptr;
	char name[256], *value;

	if (src_array == NULL)
		return;

	value = xmalloc(ENV_BUFSIZE);
	for (ptr = (char **)src_array; *ptr != NULL; ptr++) {
		if (_env_array_entry_splitter(*ptr, name, sizeof(name),
					      value, ENV_BUFSIZE) &&
		    (strncmp(name, "SLURM", 5) == 0))
			env_array_overwrite(dest_array, name, value);
	}
	xfree(value);
}

/*
 * Strip out trailing carriage returns and newlines
 */
static void _strip_cr_nl(char *line)
{
	int len = strlen(line);
	char *ptr;

	for (ptr = line+len-1; ptr >= line; ptr--) {
		if (*ptr=='\r' || *ptr=='\n') {
			*ptr = '\0';
		} else {
			return;
		}
	}
}

/* Return the net count of curly brackets in a string
 * '{' adds one and '}' subtracts one (zero means it is balanced).
 * Special case: return -1 if no open brackets are found */
static int _bracket_cnt(char *value)
{
	int count = 0, i;
	for (i=0; value[i]; i++) {
		if (value[i] == '{')
			count++;
		else if (value[i] == '}')
			count--;
	}
	return count;
}

/*
 * Load user environment from a specified file or file descriptor.
 *
 * This will read in a user specified file or fd, that is invoked
 * via the --export-file option in sbatch. The NAME=value entries must
 * be NULL separated to support special characters in the environment
 * definitions.
 *
 * (Note: This is being added to a minor release. For the
 * next major release, it might be a consideration to merge
 * this funcitonality with that of load_env_cache and update
 * env_cache_builder to use the NULL character.)
 */
char **env_array_from_file(const char *fname)
{
	char *buf = NULL, *ptr = NULL, *eptr = NULL;
	char *value, *p;
	char **env = NULL;
	char name[256];
	int buf_size = BUFSIZ, buf_left;
	int file_size = 0, tmp_size;
	int separator = '\0';
	int fd;

	/*
	 * If file name is a numeric value, then it is assumed to be a
	 * file descriptor.
	 */
	fd = (int)strtol(fname, &p, 10);
	if ((*p != '\0') || (fd < 3) || (fd > sysconf(_SC_OPEN_MAX)) ||
	    (fcntl(fd, F_GETFL) < 0)) {
		fd = open(fname, O_RDONLY);
		if (fd == -1) {
			error("Could not open user environment file %s", fname);
			return NULL;
		}
		verbose("Getting environment variables from %s", fname);
	} else
		verbose("Getting environment variables from fd %d", fd);

	/*
	 * Read in the user's environment data.
	 */
	buf = ptr = xmalloc(buf_size);
	buf_left = buf_size;
	while ((tmp_size = read(fd, ptr, buf_left))) {
		if (tmp_size < 0) {
			if (errno == EINTR)
				continue;
			error("read(environment_file): %m");
			break;
		}
		buf_left  -= tmp_size;
		file_size += tmp_size;
		if (buf_left == 0) {
			buf_size += BUFSIZ;
			xrealloc(buf, buf_size);
		}
		ptr = buf + file_size;
		buf_left = buf_size - file_size;
	}
	close(fd);

	/*
	 * Parse the buffer into individual environment variable names
	 * and build the environment.
	 */
	env   = env_array_create();
	value = xmalloc(ENV_BUFSIZE);
	for (ptr = buf; ; ptr = eptr+1) {
		eptr = strchr(ptr, separator);
		if ((ptr == eptr) || (eptr == NULL))
			break;
		if (_env_array_entry_splitter(ptr, name, sizeof(name),
					      value, ENV_BUFSIZE) &&
		    (!_discard_env(name, value))) {
			/*
			 * Unset the SLURM_SUBMIT_DIR if it is defined so
			 * that this new value does not get overwritten
			 * in the subsequent call to env_array_merge().
			 */
			if (strcmp(name, "SLURM_SUBMIT_DIR") == 0)
				unsetenv(name);
			env_array_overwrite(&env, name, value);
		}
	}
	xfree(buf);
	xfree(value);

	return env;
}

/*
 * Load user environment from a cache file located in
 * <state_save_location>/env_username
 */
static char **_load_env_cache(const char *username)
{
	char *state_save_loc, fname[MAXPATHLEN];
	char *line, name[256], *value;
	char **env = NULL;
	FILE *fp;
	int i;

	state_save_loc = slurm_get_state_save_location();
	i = snprintf(fname, sizeof(fname), "%s/env_cache/%s", state_save_loc,
		     username);
	xfree(state_save_loc);
	if (i < 0) {
		error("Environment cache filename overflow");
		return NULL;
	}
	if (!(fp = fopen(fname, "r"))) {
		error("Could not open user environment cache at %s: %m",
			fname);
		return NULL;
	}

	verbose("Getting cached environment variables at %s", fname);
	env = env_array_create();
	line  = xmalloc(ENV_BUFSIZE);
	value = xmalloc(ENV_BUFSIZE);
	while (1) {
		if (!fgets(line, ENV_BUFSIZE, fp))
			break;
		_strip_cr_nl(line);
		if (_env_array_entry_splitter(line, name, sizeof(name),
					      value, ENV_BUFSIZE) &&
		    (!_discard_env(name, value))) {
			if (value[0] == '(') {
				/* This is a bash function.
				 * It may span multiple lines */
				while (_bracket_cnt(value) > 0) {
					if (!fgets(line, ENV_BUFSIZE, fp))
						break;
					_strip_cr_nl(line);
					if ((strlen(value) + strlen(line)) >
					    (ENV_BUFSIZE - 2))
						break;
					strcat(value, "\n");
					strcat(value, line);
				}
			}
			env_array_overwrite(&env, name, value);
		}
	}
	xfree(line);
	xfree(value);

	fclose(fp);
	return env;
}

/*
 * Return an array of strings representing the specified user's default
 * environment variables following a two-prongged approach.
 * 1. Execute (more or less): "/bin/su - <username> -c /usr/bin/env"
 *    Depending upon the user's login scripts, this may take a very
 *    long time to complete or possibly never return
 * 2. Load the user environment from a cache file. This is used
 *    in the event that option 1 times out.
 *
 * timeout value is in seconds or zero for default (2 secs)
 * mode is 1 for short ("su <user>"), 2 for long ("su - <user>")
 * On error, returns NULL.
 *
 * NOTE: The calling process must have an effective uid of root for
 * this function to succeed.
 */
char **env_array_user_default(const char *username, int timeout, int mode)
{
	char *line = NULL, *last = NULL, name[MAXPATHLEN], *value, *buffer;
	char **env = NULL;
	char *starttoken = "XXXXSLURMSTARTPARSINGHEREXXXX";
	char *stoptoken  = "XXXXSLURMSTOPPARSINGHEREXXXXX";
	char cmdstr[256], *env_loc = NULL;
	char stepd_path[MAXPATHLEN];
	int fildes[2], found, fval, len, rc, timeleft;
	int buf_read, buf_rem, config_timeout;
	pid_t child;
	struct timeval begin, now;
	struct pollfd ufds;
	struct stat buf;

	if (geteuid() != (uid_t)0) {
		error("SlurmdUser must be root to use --get-user-env");
		return NULL;
	}

	snprintf(stepd_path, sizeof(stepd_path), "%s/sbin/slurmstepd",
		 SLURM_PREFIX);
	config_timeout = slurm_get_env_timeout();
	if (config_timeout == 0)	/* just read directly from cache */
		 return _load_env_cache(username);

	if (stat("/bin/su", &buf))
		fatal("Could not locate command: /bin/su");
	if (stat("/bin/echo", &buf))
		fatal("Could not locate command: /bin/echo");
	if (stat(stepd_path, &buf) == 0) {
		snprintf(name, sizeof(name), "%s getenv", stepd_path);
		env_loc = name;
	} else if (stat("/bin/env", &buf) == 0)
		env_loc = "/bin/env";
	else if (stat("/usr/bin/env", &buf) == 0)
		env_loc = "/usr/bin/env";
	else
		fatal("Could not location command: env");
	snprintf(cmdstr, sizeof(cmdstr),
		 "/bin/echo; /bin/echo; /bin/echo; "
		 "/bin/echo %s; %s; /bin/echo %s",
		 starttoken, env_loc, stoptoken);

	if (pipe(fildes) < 0) {
		fatal("pipe: %m");
		return NULL;
	}

	child = fork();
	if (child == -1) {
		fatal("fork: %m");
		return NULL;
	}
	if (child == 0) {
		setenv("ENVIRONMENT", "BATCH", 1);
		setpgid(0, 0);
		close(0);
		open("/dev/null", O_RDONLY);
		dup2(fildes[1], 1);
		close(2);
		open("/dev/null", O_WRONLY);
		if      (mode == 1)
			execl("/bin/su", "su", username, "-c", cmdstr, NULL);
		else if (mode == 2)
			execl("/bin/su", "su", "-", username, "-c", cmdstr, NULL);
		else {	/* Default system configuration */
#ifdef LOAD_ENV_NO_LOGIN
			execl("/bin/su", "su", username, "-c", cmdstr, NULL);
#else
			execl("/bin/su", "su", "-", username, "-c", cmdstr, NULL);
#endif
		}
		exit(1);
	}

	close(fildes[1]);
	if ((fval = fcntl(fildes[0], F_GETFL, 0)) < 0)
		error("fcntl(F_GETFL) failed: %m");
	else if (fcntl(fildes[0], F_SETFL, fval | O_NONBLOCK) < 0)
		error("fcntl(F_SETFL) failed: %m");

	gettimeofday(&begin, NULL);
	ufds.fd = fildes[0];
	ufds.events = POLLIN;

	/* Read all of the output from /bin/su into buffer */
	if (timeout == 0)
		timeout = config_timeout;	/* != 0 test above */
	found = 0;
	buf_read = 0;
	buffer = xmalloc(ENV_BUFSIZE);
	while (1) {
		gettimeofday(&now, NULL);
		timeleft = timeout * 1000;
		timeleft -= (now.tv_sec -  begin.tv_sec)  * 1000;
		timeleft -= (now.tv_usec - begin.tv_usec) / 1000;
		if (timeleft <= 0) {
			verbose("timeout waiting for /bin/su to complete");
			kill(-child, 9);
			break;
		}
		if ((rc = poll(&ufds, 1, timeleft)) <= 0) {
			if (rc == 0) {
				verbose("timeout waiting for /bin/su to complete");
				break;
			}
			if ((errno == EINTR) || (errno == EAGAIN))
				continue;
			error("poll(): %m");
			break;
		}
		if (!(ufds.revents & POLLIN)) {
			if (ufds.revents & POLLHUP) {	/* EOF */
				found = 1;		/* success */
			} else if (ufds.revents & POLLERR) {
				error("POLLERR");
			} else {
				error("poll() revents=%d", ufds.revents);
			}
			break;
		}
		buf_rem = ENV_BUFSIZE - buf_read;
		if (buf_rem == 0) {
			error("buffer overflow loading env vars");
			break;
		}
		rc = read(fildes[0], &buffer[buf_read], buf_rem);
		if (rc > 0)
			buf_read += rc;
		else if (rc == 0) {	/* EOF */
			found = 1;	/* success */
			break;
		} else {		/* error */
			error("read(env pipe): %m");
			break;
		}
	}
	close(fildes[0]);
	for (config_timeout=0; ; config_timeout++) {
		kill(-child, SIGKILL);	/* Typically a no-op */
		if (config_timeout)
			sleep(1);
		if (waitpid(child, &rc, WNOHANG) > 0)
			break;
		if (config_timeout >= 2) {
			/* Non-killable processes are indicative of file system
			 * problems. The process will remain as a zombie, but
			 * slurmd/salloc/moab will not otherwise be effected. */
			error("Failed to kill program loading user environment");
			break;
		}
	}

	if (!found) {
		error("Failed to load current user environment variables");
		xfree(buffer);
		return _load_env_cache(username);
	}

	/* First look for the start token in the output */
	len = strlen(starttoken);
	found = 0;
	line = strtok_r(buffer, "\n", &last);
	while (!found && line) {
		if (!strncmp(line, starttoken, len)) {
			found = 1;
			break;
		}
		line = strtok_r(NULL, "\n", &last);
	}
	if (!found) {
		error("Failed to get current user environment variables");
		xfree(buffer);
		return _load_env_cache(username);
	}

	/* Process environment variables until we find the stop token */
	len = strlen(stoptoken);
	found = 0;
	env = env_array_create();
	line = strtok_r(NULL, "\n", &last);
	value = xmalloc(ENV_BUFSIZE);
	while (!found && line) {
		if (!strncmp(line, stoptoken, len)) {
			found = 1;
			break;
		}
		if (_env_array_entry_splitter(line, name, sizeof(name),
					      value, ENV_BUFSIZE) &&
		    (!_discard_env(name, value))) {
			if (value[0] == '(') {
				/* This is a bash function.
				 * It may span multiple lines */
				while (_bracket_cnt(value) > 0) {
					line = strtok_r(NULL, "\n", &last);
					if (!line)
						break;
					if ((strlen(value) + strlen(line)) >
					    (ENV_BUFSIZE - 2))
						break;
					strcat(value, "\n");
					strcat(value, line);
				}
			}
			env_array_overwrite(&env, name, value);
		}
		line = strtok_r(NULL, "\n", &last);
	}
	xfree(value);
	xfree(buffer);
	if (!found) {
		error("Failed to get all user environment variables");
		env_array_free(env);
		return _load_env_cache(username);
	}

	return env;
}
