/*****************************************************************************\
 *  accounting_storage_slurmdbd.c - accounting interface to slurmdbd.
 *****************************************************************************
 *  Copyright (C) 2004-2007 The Regents of the University of California.
 *  Copyright (C) 2008 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
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

#include "assoc_mgr.h"

#include <sys/types.h>
#include <pwd.h>

#include "src/common/xstring.h"
#include "src/slurmdbd/read_config.h"

static List local_association_list = NULL;
static List local_user_list = NULL;
static char *local_cluster_name = NULL;

static pthread_mutex_t local_association_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t local_user_lock = PTHREAD_MUTEX_INITIALIZER;

static int _get_local_association_list(void *db_conn, int enforce)
{
	acct_association_cond_t assoc_q;
	char *cluster_name = NULL;

	slurm_mutex_lock(&local_association_lock);
	if(local_association_list)
		list_destroy(local_association_list);

	memset(&assoc_q, 0, sizeof(acct_association_cond_t));
	if(local_cluster_name) {
		assoc_q.cluster_list = list_create(slurm_destroy_char);
		cluster_name = xstrdup(local_cluster_name);
		if(!cluster_name) {
			if(enforce && !slurmdbd_conf) {
				error("_get_local_association_list: "
				      "no cluster name here going to get "
				      "all associations.");
			}
		} else 
			list_append(assoc_q.cluster_list, cluster_name);
	}

	local_association_list =
		acct_storage_g_get_associations(db_conn, &assoc_q);
	if(assoc_q.cluster_list)
		list_destroy(assoc_q.cluster_list);
	
	if(!local_association_list) {
		slurm_mutex_unlock(&local_association_lock);
		if(enforce) {
			error("_get_local_association_list: "
			      "no list was made.");
			return SLURM_ERROR;
		} else {
			return SLURM_SUCCESS;
		}
	} else {
		acct_association_rec_t *assoc = NULL;
		struct passwd *passwd_ptr = NULL;
		ListIterator itr = list_iterator_create(local_association_list);
		while((assoc = list_next(itr))) {
			if(!assoc->user)
				continue;
			passwd_ptr = getpwnam(assoc->user);
			if(passwd_ptr) 
				assoc->uid = passwd_ptr->pw_uid;
		}
		list_iterator_destroy(itr);
	}
	slurm_mutex_unlock(&local_association_lock);

	return SLURM_SUCCESS;
}

static int _get_local_user_list(void *db_conn, int enforce)
{
	acct_user_cond_t user_q;

	memset(&user_q, 0, sizeof(acct_user_cond_t));

	slurm_mutex_lock(&local_user_lock);
	if(local_user_list)
		list_destroy(local_user_list);
	local_user_list = acct_storage_g_get_users(db_conn, &user_q);

	if(!local_user_list) {
		slurm_mutex_unlock(&local_user_lock);
		if(enforce) {
			error("_get_local_user_list: "
			      "no list was made.");
			return SLURM_ERROR;
		} else {
			return SLURM_SUCCESS;
		}		
	} 

	slurm_mutex_unlock(&local_user_lock);
	return SLURM_SUCCESS;
}

extern int assoc_mgr_init(void *db_conn, int enforce)
{
	if(!local_cluster_name && !slurmdbd_conf)
		local_cluster_name = slurm_get_cluster_name();

	if(!local_association_list) 
		if(_get_local_association_list(db_conn, enforce) == SLURM_ERROR)
			return SLURM_ERROR;

	if(!local_user_list) 
		if(_get_local_user_list(db_conn, enforce) == SLURM_ERROR)
			return SLURM_ERROR;

	return SLURM_SUCCESS;
}

extern int assoc_mgr_fini()
{
	if(local_association_list) 
		list_destroy(local_association_list);
	if(local_user_list)
		list_destroy(local_user_list);
	xfree(local_cluster_name);
	local_association_list = NULL;
	local_user_list = NULL;

	return SLURM_SUCCESS;
}

extern int assoc_mgr_fill_in_assoc(void *db_conn, acct_association_rec_t *assoc,
				   int enforce)
{
	ListIterator itr = NULL;
	acct_association_rec_t * found_assoc = NULL;
	acct_association_rec_t * ret_assoc = NULL;
	
	if(!local_association_list) 
		if(_get_local_association_list(db_conn, enforce) == SLURM_ERROR)
			return SLURM_ERROR;

	if((!local_association_list || !list_count(local_association_list))
	   && !enforce) 
		return SLURM_SUCCESS;

	if(!assoc->id) {
		if(!assoc->acct) {
			acct_user_rec_t user;

			if(!assoc->uid) {
				if(enforce) {
					error("get_assoc_id: "
					      "Not enough info to "
					      "get an association");
					return SLURM_ERROR;
				} else {
					return SLURM_SUCCESS;
				}
			}
			memset(&user, 0, sizeof(acct_user_rec_t));
			user.uid = assoc->uid;
			if(assoc_mgr_fill_in_user(db_conn, &user, enforce) 
			   == SLURM_ERROR) {
				if(enforce) 
					return SLURM_ERROR;
				else {
					return SLURM_SUCCESS;
				}
			}					
			assoc->user = user.name;
			assoc->acct = user.default_acct;
		} 
		
		if(!assoc->cluster)
			assoc->cluster = local_cluster_name;
	}
/* 	info("looking for assoc of user=%u, acct=%s, cluster=%s, partition=%s", */
/* 	     assoc->uid, assoc->acct, assoc->cluster, assoc->partition); */
	slurm_mutex_lock(&local_association_lock);
	itr = list_iterator_create(local_association_list);
	while((found_assoc = list_next(itr))) {
		if(assoc->id) {
			if(assoc->id == found_assoc->id) {
				ret_assoc = found_assoc;
				break;
			}
			continue;
		} else {
			if(!assoc->user && found_assoc->user) {
				debug3("we are looking for a "
				       "nonuser association");
				continue;
			} else if(assoc->uid != found_assoc->uid) {
				debug3("not the right user");
				continue;
			}
			
			if(found_assoc->acct 
			   && strcasecmp(assoc->acct, found_assoc->acct)) {
				   debug3("not the right account");
				   continue;
			}

			/* only check for on the slurmdbd */
			if(!local_cluster_name && found_assoc->cluster
			   && strcasecmp(assoc->cluster,
					 found_assoc->cluster)) {
				debug3("not the right cluster");
				continue;
			}
	
			if(assoc->partition
			   && (!found_assoc->partition 
			       || strcasecmp(assoc->partition, 
					     found_assoc->partition))) {
				ret_assoc = found_assoc;
				debug3("found association for no partition");
				continue;
			}
		}
		debug3("found correct association");
		ret_assoc = found_assoc;
		break;
	}
	list_iterator_destroy(itr);
	
	if(!ret_assoc) {
		slurm_mutex_unlock(&local_association_lock);
		if(enforce) 
			return SLURM_ERROR;
		else
			return SLURM_SUCCESS;
	}

	assoc->id = ret_assoc->id;
	if(!assoc->user)
		assoc->user = ret_assoc->user;
	if(!assoc->acct)
		assoc->acct = ret_assoc->acct;
	if(!assoc->cluster)
		assoc->cluster = ret_assoc->cluster;
	if(!assoc->partition)
		assoc->partition = ret_assoc->partition;
	slurm_mutex_unlock(&local_association_lock);

	return SLURM_SUCCESS;
}

extern int assoc_mgr_fill_in_user(void *db_conn, acct_user_rec_t *user,
				  int enforce)
{
	ListIterator itr = NULL;
	acct_user_rec_t * found_user = NULL;

	if(!local_user_list) 
		if(_get_local_user_list(db_conn, enforce) == SLURM_ERROR)
			return SLURM_ERROR;

	if((!local_user_list || !list_count(local_user_list)) && !enforce) 
		return SLURM_SUCCESS;

	slurm_mutex_lock(&local_user_lock);
	itr = list_iterator_create(local_user_list);
	while((found_user = list_next(itr))) {
		if(user->uid == found_user->uid) 
			break;
	}
	list_iterator_destroy(itr);

	if(found_user) {
		memcpy(user, found_user, sizeof(acct_user_rec_t));		
		slurm_mutex_unlock(&local_user_lock);
		return SLURM_SUCCESS;
	}
	slurm_mutex_unlock(&local_user_lock);
	return SLURM_ERROR;
}

extern acct_admin_level_t assoc_mgr_get_admin_level(void *db_conn,
						    uint32_t uid)
{
	ListIterator itr = NULL;
	acct_user_rec_t * found_user = NULL;

	if(!local_user_list) 
		if(_get_local_user_list(db_conn, 0) == SLURM_ERROR)
			return ACCT_ADMIN_NOTSET;

	if(!local_user_list) 
		return ACCT_ADMIN_NOTSET;

	slurm_mutex_lock(&local_user_lock);
	itr = list_iterator_create(local_user_list);
	while((found_user = list_next(itr))) {
		if(uid == found_user->uid) 
			break;
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&local_user_lock);
		
	if(found_user) 
		return found_user->admin_level;
		
	return ACCT_ADMIN_NOTSET;	
}

extern int assoc_mgr_is_user_acct_coord(void *db_conn,
					uint32_t uid,
					char *acct_name)
{
	ListIterator itr = NULL;
	acct_coord_rec_t *acct = NULL;
	acct_user_rec_t * found_user = NULL;

	if(!local_user_list) 
		if(_get_local_user_list(db_conn, 0) == SLURM_ERROR)
			return ACCT_ADMIN_NOTSET;

	if(!local_user_list) 
		return ACCT_ADMIN_NOTSET;

	slurm_mutex_lock(&local_user_lock);
	itr = list_iterator_create(local_user_list);
	while((found_user = list_next(itr))) {
		if(uid == found_user->uid) 
			break;
	}
	list_iterator_destroy(itr);
		
	if(!found_user) {
		slurm_mutex_unlock(&local_user_lock);
		return 0;
	}
	itr = list_iterator_create(found_user->coord_accts);
	while((acct = list_next(itr))) {
		if(!strcmp(acct_name, acct->acct_name))
			break;
	}
	list_iterator_destroy(itr);
	
	if(acct) {
		slurm_mutex_unlock(&local_user_lock);
		return 1;
	}
	slurm_mutex_unlock(&local_user_lock);

	return 0;	
}

extern int assoc_mgr_update_local_assocs(acct_update_object_t *update)
{
	acct_association_rec_t * rec = NULL;
	acct_association_rec_t * object = NULL;
	ListIterator itr = NULL;
	int rc = SLURM_SUCCESS;

	if(!local_association_list)
		return SLURM_SUCCESS;

	slurm_mutex_lock(&local_association_lock);
	itr = list_iterator_create(local_association_list);
	while((object = list_pop(update->objects))) {
		if(local_cluster_name) {
			/* only update the local clusters assocs */
			if(strcasecmp(object->cluster, local_cluster_name))
				continue;
		}
		list_iterator_reset(itr);
		while((rec = list_next(itr))) {
			if(object->id) {
				if(object->id == rec->id) {
					break;
				}
				continue;
			} else {
				if(!object->user && rec->user) {
					debug3("we are looking for a "
					       "nonuser association");
					continue;
				} else if(object->uid != rec->uid) {
					debug3("not the right user");
					continue;
				}
				
				if(object->acct
				   && (!rec->acct 
				       || strcasecmp(object->acct,
						     rec->acct))) {
					debug3("not the right account");
					continue;
				}
				
				/* only check for on the slurmdbd */
				if(!local_cluster_name && object->acct
				   && (!rec->cluster
				       || strcasecmp(object->cluster,
						     rec->cluster))) {
					debug3("not the right cluster");
					continue;
				}
				
				if(object->partition
				   && (!rec->partition 
				       || strcasecmp(object->partition, 
						     rec->partition))) {
					debug3("not the right partition");
					continue;
				}
				break;
			}			
		}
		//info("%d assoc %u", update->type, object->id);
		switch(update->type) {
		case ACCT_MODIFY_ASSOC:
			if(!rec) {
				//rc = SLURM_ERROR;
				break;
			}
			debug("updating the assocs here on %u", rec->id);
			if((int)object->fairshare >= -1) {
				rec->fairshare = object->fairshare;
			}

			if((int)object->max_jobs >= -1) {
				rec->max_jobs = object->max_jobs;
			}

			if((int)object->max_nodes_per_job >= -1) {
				rec->max_nodes_per_job =
					object->max_nodes_per_job;
			}

			if((int)object->max_wall_duration_per_job >= -1) {
				rec->max_wall_duration_per_job =
					object->max_wall_duration_per_job;
			}

			if((int)object->max_cpu_secs_per_job >= -1) {
				rec->max_cpu_secs_per_job = 
					object->max_cpu_secs_per_job;
			}

			if(object->parent_acct) {
				xfree(rec->parent_acct);
				rec->parent_acct = xstrdup(object->parent_acct);
			}

			/* FIX ME: do more updates here */
			break;
		case ACCT_ADD_ASSOC:
			if(rec) {
				//rc = SLURM_ERROR;
				break;
			}
			list_append(local_association_list, object);
		case ACCT_REMOVE_ASSOC:
			if(!rec) {
				//rc = SLURM_ERROR;
				break;
			}
			list_delete_item(itr);
			break;
		default:
			break;
		}
		if(update->type != ACCT_ADD_ASSOC) {
			destroy_acct_update_object(object);			
		}
				
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&local_association_lock);

	return rc;	
}

extern int assoc_mgr_update_local_users(acct_update_object_t *update)
{
	acct_user_rec_t * rec = NULL;
	acct_user_rec_t * object = NULL;
	ListIterator itr = NULL;
	int rc = SLURM_SUCCESS;

	if(!local_user_list)
		return SLURM_SUCCESS;

	slurm_mutex_lock(&local_user_lock);
	itr = list_iterator_create(local_user_list);
	while((object = list_pop(update->objects))) {
		list_iterator_reset(itr);
		while((rec = list_next(itr))) {
			if(!strcasecmp(object->name, rec->name)) {
				break;
			}
		}
		//info("%d user %s", update->type, object->name);
		switch(update->type) {
		case ACCT_MODIFY_USER:
			if(!rec) {
				rc = SLURM_ERROR;
				break;
			}

			if(object->default_acct) {
				xfree(rec->default_acct);
				rec->default_acct = object->default_acct;
				object->default_acct = NULL;
			}

			if(object->qos != ACCT_QOS_NOTSET)
				rec->qos = object->qos;
			
			if(object->admin_level != ACCT_ADMIN_NOTSET)
				rec->admin_level = rec->admin_level;

			break;
		case ACCT_ADD_USER:
			if(rec) {
				rc = SLURM_ERROR;
				break;
			}
			list_append(local_user_list, object);
		case ACCT_REMOVE_USER:
			if(!rec) {
				rc = SLURM_ERROR;
				break;
			}
			list_delete_item(itr);
			break;
		default:
			break;
		}
		if(update->type != ACCT_ADD_USER) {
			destroy_acct_update_object(object);			
		}
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&local_user_lock);

	return rc;	
}

extern int assoc_mgr_validate_assoc_id(void *db_conn,
				       uint32_t assoc_id,
				       int enforce)
{
	ListIterator itr = NULL;
	acct_association_rec_t * found_assoc = NULL;

	if(!local_association_list) 
		if(_get_local_association_list(db_conn, enforce) == SLURM_ERROR)
			return SLURM_ERROR;

	if((!local_association_list || !list_count(local_association_list))
	   && !enforce) 
		return SLURM_SUCCESS;
	
	slurm_mutex_lock(&local_association_lock);
	itr = list_iterator_create(local_association_list);
	while((found_assoc = list_next(itr))) {
		if(assoc_id == found_assoc->id) 
			break;
	}
	list_iterator_destroy(itr);
	slurm_mutex_unlock(&local_association_lock);

	if(found_assoc || !enforce)
		return SLURM_SUCCESS;

	return SLURM_ERROR;
}

