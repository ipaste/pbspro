/*
 * Copyright (C) 1994-2016 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *  
 * This file is part of the PBS Professional ("PBS Pro") software.
 * 
 * Open Source License Information:
 *  
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free 
 * Software Foundation, either version 3 of the License, or (at your option) any 
 * later version.
 *  
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY 
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.
 *  
 * You should have received a copy of the GNU Affero General Public License along 
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *  
 * Commercial License Information: 
 * 
 * The PBS Pro software is licensed under the terms of the GNU Affero General 
 * Public License agreement ("AGPL"), except where a separate commercial license 
 * agreement for PBS Pro version 14 or later has been executed in writing with Altair.
 *  
 * Altair’s dual-license business model allows companies, individuals, and 
 * organizations to create proprietary derivative works of PBS Pro and distribute 
 * them - whether embedded or bundled with other software - under a commercial 
 * license agreement.
 * 
 * Use of Altair’s trademarks, including but not limited to "PBS™", 
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's 
 * trademark licensing policies.
 *
 */
#include <pbs_config.h>   /* the master config generated by configure */
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <io.h>
#include <windows.h>
#include "win.h"
#include "pbs_ifl.h"
#include "list_link.h"
#include "attribute.h"
#include "job.h"
#include "credential.h"
#include "ticket.h"
#include "libpbs.h"
#include "batch_request.h"
#include "pbs_version.h"
#include "pbs_ecl.h"
#include "net_connect.h"
#include  "pbs_nodes.h"
#include "mom_func.h"
#include "log.h"

/**
 * @file	mom_stage_file.c
 */
/* Global Data Items */
char *path_log = NULL;
char *path_spool = NULL;
char *path_undeliv = NULL;
char *path_checkpoint = NULL;
char *path_jobs = NULL;
char *log_file = NULL;
time_t time_now = 0;
char rcperr[MAXPATHLEN] = {'\0'};	/* file to contain rcp error */
char *pbs_jobdir = NULL;		/* path to staging and execution dir of current job */
char *cred_buf = NULL;
size_t cred_len = 0;
char mom_host[PBS_MAXHOSTNAME+1] = {'\0'};
struct cphosts *pcphosts = 0;
int cphosts_num = 0;

/**
 * @brief
 * 	main - the initialization and main loop of pbs_stage_file
 */
int
main(int argc, char *argv[])
{
	static char id[] = "pbs_stage_file";
	char buf[CPY_PIPE_BUFSIZE] = {'\0'};
	char *param_name = NULL;
	char *param_val = NULL;
	int rc = -1;
	time_t copy_start = 0;
	time_t copy_stop = 0;
	int dir = 0;
	int num_copies = 0;
	struct rqfpair *pair = NULL;
	int i = -1;
	int rmtflag = -1;
	struct rq_cpyfile rqcpf_buf = {0};
	struct rq_cpyfile *rqcpf = &rqcpf_buf;
	struct passwd *pw = NULL;
	char *actual_homedir = NULL;
	cpy_files stage_inout = {0};
	char *prmt = NULL;

	execution_mode(argc, argv);

	if(set_msgdaemonname("PBS_stage_file")) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}

	pbs_loadconf(0);

	pbs_client_thread_set_single_threaded_mode();
	/* disable attribute verification */
	set_no_attribute_verification();

	/* initialize the thread context */
	if (pbs_client_thread_init_thread_context() != 0) {
		printf("Unable to initialize thread context\n");
		exit(STAGEFILE_FATAL);
	}

	winsock_init();

	connection_init();

	while (fgets(buf, sizeof(buf), stdin) != NULL) {
		buf[strlen(buf)-1] = '\0';	/* gets rid of newline */

		param_name = buf;
		param_val = strchr(buf, '=');
		if (param_val) {
			*param_val = '\0';
			param_val++;
		} else {	/* bad param_val */
			break;
		}

		if (strcmp(param_name, "path_log") == 0) {
			path_log = strdup(param_val);
		} else if (strcmp(param_name, "path_spool") == 0) {
			path_spool = strdup(param_val);
		} else if (strcmp(param_name, "path_undeliv") == 0) {
			path_undeliv = strdup(param_val);
		} else if (strcmp(param_name, "path_checkpoint") == 0) {
			path_checkpoint = strdup(param_val);
		} else if (strcmp(param_name, "pbs_jobdir") == 0) {
			pbs_jobdir = strdup(param_val);
		} else if (strcmp(param_name, "actual_homedir") == 0) {
			actual_homedir = strdup(param_val);
		} else if (strcmp(param_name, "mom_host") == 0) {
			strncpy_s(mom_host, sizeof(mom_host), param_val, _TRUNCATE);
		} else if (strcmp(param_name, "log_file") == 0) {
			log_file = strdup(param_val);
		} else if (strcmp(param_name, "pcphosts") == 0) {
			if (!recv_pcphosts()) {
				printf("error while receiving pcphosts\n");
				if (actual_homedir)
					unmap_unc_path(actual_homedir);
				net_close(-1);
				exit(STAGEFILE_FATAL);
			}
		} else if (strcmp(param_name, "rq_cpyfile") == 0) {
			if (!recv_rq_cpyfile_cred(rqcpf)) {
				printf("error while receiving rq_cpyfile info and cred\n");
				if (actual_homedir)
					unmap_unc_path(actual_homedir);
				net_close(-1);
				exit(STAGEFILE_FATAL);
			}
		} else {
			printf("unrecognized parameter\n");
			exit(STAGEFILE_FATAL);
		}
	}

	if ((path_log == NULL) ||
		(path_spool == NULL) ||
		(path_undeliv == NULL) ||
		(path_checkpoint == NULL) ||
		(pbs_jobdir == NULL) ||
		(actual_homedir == NULL) ||
		(*mom_host == '\0') ||
		(log_file == NULL)) {
		printf("error in one or more the parameters\n");
		if (actual_homedir)
			unmap_unc_path(actual_homedir);
		net_close(-1);
		exit(STAGEFILE_FATAL);
	}

	time(&time_now);

	(void)log_open_main(log_file, path_log, 1); /* silent open */

	if ((cred_len > 0) && (cred_buf != NULL)) {
		pw = logon_pw(rqcpf->rq_user, cred_buf, cred_len, pbs_decrypt_pwd, 0, log_buffer);
		log_event(PBSEVENT_DEBUG2, PBS_EVENTCLASS_JOB, LOG_DEBUG, rqcpf->rq_jobid, log_buffer);
		if ((pw != NULL) && (pw->pw_userlogin != INVALID_HANDLE_VALUE)) {
			if (!impersonate_user(pw->pw_userlogin)) {
				snprintf(log_buffer, sizeof(log_buffer), "ImpersonateLoggedOnUser failed for %s", rqcpf->rq_user);
				log_err(-1, id, log_buffer);
				unmap_unc_path(actual_homedir);
				log_close(0);	/* silent close */
				net_close(-1);
				exit(STAGEFILE_BADUSER);
			}
		} else {
			SetLastError(ERROR_INVALID_HANDLE);
			snprintf(log_buffer, sizeof(log_buffer), "logon_pw failed for %s", rqcpf->rq_user);
			log_err(-1, id, log_buffer);
			unmap_unc_path(actual_homedir);
			log_close(0);	/* silent close */
			net_close(-1);
			exit(STAGEFILE_BADUSER);
		}
	}

	dir  = (rqcpf->rq_dir & STAGE_DIRECTION)? STAGE_DIR_OUT : STAGE_DIR_IN;
	stage_inout.sandbox_private = (rqcpf->rq_dir & STAGE_JOBDIR)? TRUE : FALSE;

	if (stage_inout.sandbox_private) {
		/* chdir to job staging and execution directory if
		 * "PRIVATE" or "O_WORKDIR" mode is requested
		 */
		chdir(pbs_jobdir);
	} else {
		/* chdir to user's home directory */
		(void)chdir(actual_homedir);
	}

	/*
	 * Now running in the user's home or job directory as the user.
	 * Build up cp/rcp command(s), one per file pair
	 */

	copy_start = time(0);
	for (pair=(struct rqfpair *)GET_NEXT(rqcpf->rq_pair);
		pair != 0;
		pair = (struct rqfpair *)GET_NEXT(pair->fp_link)) {

		stage_inout.from_spool = 0;
		prmt = pair->fp_rmt;
		num_copies++;

		if (local_or_remote(&prmt) == 0) {
			/* destination host is this host, use cp */
			rmtflag = 0;
		} else {
			/* destination host is another, use (pbs_)rcp */
			rmtflag = 1;
		}

		rc = stage_file(dir, rmtflag, rqcpf->rq_owner,
			pair, 0, &stage_inout, prmt);
		if (rc != 0)
			break;
	}
	copy_stop = time(0);

	/*
	 * If there was a stage in failure, remove the job directory.
	 * There is no guarantee we'll run on this mom again,
	 * So we need to cleanup.
	 */
	if ((dir == STAGE_DIR_IN) && stage_inout.sandbox_private && stage_inout.bad_files) {
		/* cd to user's home to be out of   */
		/* the sandbox so it can be deleted */
		chdir(actual_homedir);
		rmjobdir(rqcpf->rq_jobid, pbs_jobdir, NULL, NULL);
	}

	/* log the number of files/directories copied and the time it took */
	copy_stop = copy_stop - copy_start;
	sprintf(log_buffer, "staged %d items %s over %d:%02d:%02d",
		num_copies, (dir == STAGE_DIR_OUT) ? "out" : "in",
		copy_stop/3600, (copy_stop%3600)/60, copy_stop%60);
	log_event(PBSEVENT_DEBUG2, PBS_EVENTCLASS_JOB, LOG_DEBUG,
		rqcpf->rq_jobid, log_buffer);

	if ((stage_inout.bad_files) || (stage_inout.sandbox_private && stage_inout.stageout_failed)) {
		if (stage_inout.bad_files) {
			printf("%s\n", stage_inout.bad_list);
		}
		unmap_unc_path(actual_homedir);
		log_close(0);	/* silent close */
		net_close(-1);
		exit(STAGEFILE_NOCOPYFILE);
	}

	unmap_unc_path(actual_homedir);
	log_close(0);	/* silent close */
	net_close(-1);
	exit(STAGEFILE_OK);
}
