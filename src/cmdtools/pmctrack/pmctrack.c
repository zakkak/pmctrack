/*
 * pmctrack.c
 *
 ******************************************************************************
 *
 * Copyright (c) 2015 Juan Carlos Saez, Guillermo Martinez Fernandez,
 *	 Sergio Sanchez Gordo and Sofia Dronda Merino
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 ******************************************************************************
 *
 *  2015-05-10  Modified by Abel Serrano to simplify code with invocations
 *				to libpmctrack functions.
 *  2015-08-10  Modified by Juan Carlos Saez to include support for mnemonic-based
 *				event configurations and system-wide monitoring mode.
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <wait.h>
#include <sys/resource.h>
#include <err.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sched.h>
#include <inttypes.h>
#include <pmc_user.h> /*For the data type */
#include <sys/time.h> /* For setitimer */
#include <pmctrack_internal.h>

#ifndef  _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef NO_CPU_BINDING
#define NO_CPU_BINDING 0xffffffff
#endif

 /* Various flag values for the "flags" field in struct options */
#define CMD_FLAG_ACUM_SAMPLES  (1<<0)
#define CMD_FLAG_EXTENDED_OUTPUT (1<<1)
#define CMD_FLAG_RAW_PMC_FORMAT (1<<2)
#define CMD_FLAG_LEGACY_OUTPUT (1<<3)
#define CMD_FLAG_SHOW_CHILD_TIMES (1<<4)
#define CMD_FLAG_VIRT_COUNTER_MNEMONICS (1<<5)
#define CMD_FLAG_KERNEL_DRIVES_PMCS (1<<6)

/* 
 * Structure to store information about command-line options 
 * specified by the user
 */
struct options {
	/* Global switches */
	int timeout_secs;	
	int msecs;	
	int max_samples;
	int kernel_buffer_size;
	unsigned long cpumask;	
	int optind;
	char** argv;
	unsigned long flags; 
	/* Information on PMCs  */
	char* user_cfg_str[MAX_COUNTER_CONFIGS];
	char* strcfg[MAX_RAW_COUNTER_CONFIGS_SAFE];
	int user_nr_configs;
	unsigned int pmu_id;
	counter_mapping_t event_mapping[MAX_PERFORMANCE_COUNTERS];
	unsigned int global_pmcmask;	
	/* Information on virtual counters */
	char* virtcfg;
	unsigned int  nr_virtual_counters;
	unsigned int  virtual_mask;
};


/*
 * Variables to control process termination
 * signal handling, and global statistics
 */
pid_t pid;
int stop_profiling, file_flag;
volatile int child_finished=0;
int child_status=0;
int profile_started=0;
unsigned int ebs_on=0; 
int extended_output=0;
FILE *fo;
struct rusage child_rusage;
struct timeval start_time, end_time;

#ifndef USE_VFORK
sem_t* sem_config_ready;
#endif

#define MAX_THREADS_APP 256

/* To implement cummulative mode */
struct pid_ctrl {
	/* PID of the particular thread we're tracking
		or CPU in the system-wide monitoring mode  */
	pid_t pid;
	/* Experiment mask */
	unsigned long exp_mask;
	/* To keep track of the number of samples accumulated */
	unsigned int nr_samples_accum[MAX_COUNTER_CONFIGS];
};

inline void sigalarm_handler(int signo) {};
void sigchld_handler(int signo);
void sigint_handler(int signo);
static void usage(const char* program_name,int status);

#ifndef USE_VFORK
static void init_posix_semaphore(sem_t** sem, int value)
{
	/* create shared memory region */
	key_t segid = 50000;
	int shmid;
	void* sh_mem;
	sem_t* semaphore;

	shmid = shmget(segid, sizeof(sem_t), IPC_CREAT | 0660);
	sh_mem = shmat(shmid, (void*) 0, 0);

	/* create POSIX semaphore */
	semaphore = (sem_t*) sh_mem;
	sem_init(semaphore, 1, value);
	(*sem)=semaphore;
}
#endif

/* Included due to an issue with header files in some Linux distributions */
extern int sched_setaffinity(pid_t pid, unsigned int len, unsigned long *mask);


/* Turn a string (hex format) into a 64-bit CPU mask */
static unsigned long str_to_cpumask(const char* str)
{
	unsigned long val=0;
	if (strncmp(str,"0x",2)==0) {
		/* Remove the 0x preffix and convert to the mask directly */
		return strtol(str+2,NULL,16);
	} else {
		/* Assume just a CPU number in decimal format */
		val=strtol(str,NULL,10);
		return 0x1<<val;
	}
}

/* Wrapper for sched_setaffinity */
static inline void bind_process_cpumask(int pid, unsigned long cpumask)
{
	if ((cpumask!=NO_CPU_BINDING) && sched_setaffinity(pid, sizeof(unsigned long),&cpumask)!=0) {
		warnx("Error when binding process to cpumask 0x%lu",cpumask);
		exit(1);
	}
}

/* 
 * When using vfork(), it is strongly advisable to use _exit() rather than exit()
 * as stated in the vfork() man page.
 */
static inline void pmctrack_exit(int status)
{
#ifdef USE_VFORK
	_exit(status);
#else
	exit(status);
#endif
}

/* Sets up a timer that fires after a certain number of msecs */
static unsigned int alarm_ms(unsigned int mseconds)
{
	struct itimerval new;
	new.it_value.tv_usec = (mseconds*1000) % 1000000;
	new.it_value.tv_sec = mseconds/1000;
	new.it_interval=new.it_value;

	if (setitimer (ITIMER_REAL, &new, NULL) < 0)
		return 0;
	return 1;
}

/* 
 * This function takes care of printing event-to-counter mappings  
 * in the event the user did not specified PMC and virtual-counter
 * configurations using the raw format (kernel)
 * */
static void print_counter_mappings(FILE* fout,
                                   struct options* opts,
                                   unsigned int nr_experiments)
{

	/* Do not show anything under these circumstances */
	if ((opts->flags & (CMD_FLAG_LEGACY_OUTPUT)) ||
	    ((opts->flags & (CMD_FLAG_RAW_PMC_FORMAT|CMD_FLAG_KERNEL_DRIVES_PMCS) &&
	      !(opts->flags & CMD_FLAG_VIRT_COUNTER_MNEMONICS)) ) )
		return ;


	fprintf(fout,"[Event-to-counter mappings]\n");
	if (!(opts->flags & (CMD_FLAG_RAW_PMC_FORMAT|CMD_FLAG_KERNEL_DRIVES_PMCS)))
		pmct_print_counter_mappings(fout,opts->event_mapping,
		                            opts->global_pmcmask,
		                            nr_experiments);
	if (opts->flags & CMD_FLAG_VIRT_COUNTER_MNEMONICS)
		pmct_print_selected_virtual_counters(fout,opts->virtual_mask);
	fprintf(fout,"[Event counts]\n");

}


/*
 *  Returns non-zero if the child process has been running longer 
 * than the allowed time period (timeout) 
 **/
static int check_timeout(int timeout_secs)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	return ((now.tv_sec - start_time.tv_sec) >= timeout_secs);
}

/* Print a summary with the process times */
static void print_process_statistics(FILE* fout,
                                     struct rusage* rusage,
                                     struct timeval* start,
                                     struct timeval* end)
{
	end->tv_sec -= start->tv_sec;
	/* Handle overflow with usecs */
	if (end->tv_usec < start->tv_usec) {
		end->tv_usec += 1000000;
		--end->tv_sec;
	}
	end->tv_usec -= start->tv_usec;

	fprintf(fout,"[Process times]\n");
	/* Completion time */
	if (end->tv_sec >= 3600)
		fprintf (fout, "real\t%ld:%02ld:%02ld\n", /* Format: H:M:S  */
		         end->tv_sec / 3600,
		         (end->tv_sec % 3600) / 60,
		         end->tv_sec % 60);
	else
		fprintf (fout, "real\t%ld:%02ld.%02ld\n",	/* Format: M:S.D  */
		         end->tv_sec / 60,
		         end->tv_sec % 60,
		         end->tv_usec / 10000);

	/* User time.  */
	fprintf (fout, "user\t%ld.%02ld\n",
	         rusage->ru_utime.tv_sec,
	         rusage->ru_utime.tv_usec / 10000);

	/* System time */
	fprintf (fout, "sys\t%ld.%02ld\n",
	         rusage->ru_stime.tv_sec,
	         rusage->ru_stime.tv_usec / 10000);
}


/* Install basic signal handlers for a safe execution */
static int install_signal_handlers(void)
{
	struct sigaction sact;

	/* processing SIGALRM signal */
	sact.sa_handler = sigalarm_handler;
	sact.sa_flags = 0;
	sigemptyset(&sact.sa_mask);
	sigaddset(&sact.sa_mask, SIGALRM);
	if(sigaction(SIGALRM, &sact, NULL) < 0) {
		perror("Can't assign signal handler for SIGALRM");
		return 1;
	}

	/* processing SIGINT signal */
	sact.sa_handler = sigint_handler;
	sact.sa_flags = 0;
	sigemptyset(&sact.sa_mask);
	sigaddset(&sact.sa_mask, SIGINT);
	if(sigaction(SIGINT, &sact, NULL) < 0) {
		perror("Can't assign signal handler for SIGINT");
		return 1;
	}

	/* processing SIGTERM signal */
	sact.sa_handler = sigint_handler;
	sact.sa_flags = 0;
	sigemptyset(&sact.sa_mask);
	sigaddset(&sact.sa_mask, SIGTERM);
	if(sigaction(SIGTERM, &sact, NULL) < 0) {
		perror("Can't assign signal handler for SIGTERM");
		return 1;
	}

	/* processing SIGCHLD signal */
	sact.sa_handler = sigchld_handler;
	sact.sa_flags = 0;
	sigemptyset(&sact.sa_mask);
	sigaddset(&sact.sa_mask, SIGCHLD);
	if(sigaction(SIGCHLD, &sact, NULL) < 0) {
		perror("Can't assign signal handler for SIGHCLD");
		return 1;
	}
	return 0;
}

/* Config & Monitoring function for per-thread monitoring mode*/
static void monitoring_counters(struct options* opts,int optind,char** argv)
{
	unsigned int nr_virtual_counters=0,virtual_mask=0;
	int i, cont;
	unsigned int pmcmask=0;
	unsigned int npmcs = 0;
	int fd;
	struct timespec;
	pmc_sample_t* samples=NULL;
	pmc_sample_t** acum_samples=NULL;
	struct pid_ctrl* pid_ctrl_vector=NULL;
	int nr_pids=0;
	int nr_samples;
	unsigned int nr_experiments;
	unsigned int max_buffer_samples;

	cont = 1;
	fd = -1;
	child_status = 0;
	nr_virtual_counters=opts->nr_virtual_counters;
	virtual_mask=opts->virtual_mask;


	if (install_signal_handlers())
		exit(1);

	/* Gather info to build header */
	if (pmct_check_counter_config((const char**)opts->strcfg,&npmcs,&pmcmask,&ebs_on,&nr_experiments))
		exit(1);

	if (!opts->strcfg[0] && npmcs!=0)
		opts->flags |= CMD_FLAG_KERNEL_DRIVES_PMCS;

	if (nr_virtual_counters==0 && npmcs==0) {
		fprintf(stderr,"Please specify events to monitor!\n");
		exit(1);
	}

	if (opts->flags & CMD_FLAG_ACUM_SAMPLES) {
		acum_samples=malloc(sizeof(pmc_sample_t*)*MAX_THREADS_APP);
		pid_ctrl_vector=malloc(sizeof(struct pid_ctrl)*MAX_THREADS_APP); /* As much as 256 threads */
		if (!acum_samples || ! pid_ctrl_vector) {
			fprintf(stderr, "%s\n", "Couldn't reserve memory for cummulative counters");
			exit(1);
		}
		memset(acum_samples,0,sizeof(pmc_sample_t*)*MAX_THREADS_APP);/* Set NULL POINTERS*/
	}


#ifndef USE_VFORK
	/* init semaphore to signal the parent that the child proccess has successfuly configured the counters */
	init_posix_semaphore(&sem_config_ready,0);
#endif
	/* Flag this stuff */
	profile_started=0;
	child_finished=0;
	stop_profiling = 0;

	/* Keep track of start time */
	gettimeofday(&start_time, NULL);

#ifdef USE_VFORK
	pid = vfork();
#else
	pid = fork();
#endif
	if(pid == -1) {
		err(1,"Error forking process.");
	} else if(pid == 0) {
		// CHILD CODE:
		/* Bind first */
		bind_process_cpumask(getpid(),opts->cpumask);

		/* Set up kernel buffer size */
		if (opts->kernel_buffer_size!=-1 && pmct_set_kernel_buffer_size(opts->kernel_buffer_size))
			pmctrack_exit(1);

		/* Configure counters if there is something to configure */
		if (opts->strcfg[0] && pmct_config_counters((const char**)opts->strcfg,0))
			pmctrack_exit(1);

		/* Set up sampling period
			(check whether the kernel control the counters or not)
		*/
		if (pmct_config_timeout(opts->msecs,(!(opts->strcfg)[0] && npmcs!=0)))
			pmctrack_exit(1);

		if (opts->virtcfg && pmct_config_virtual_counters(opts->virtcfg,0))
			pmctrack_exit(1);

		/* Enable profiling !! */
		if (pmct_start_counting())
			pmctrack_exit(1);

#ifndef USE_VFORK
		/* Notify the parent that the configuration is done */
		sem_post(sem_config_ready);
#endif
		if(execvp(argv[optind], &argv[optind]) < 0) {
			warnx( "Error when trying to execute program %s\n", argv[optind] );
			pmctrack_exit(-1);
		}
	} else {	// PARENT CODE:

		if (pmct_attach_process(pid) < 0)
			exit(-1);

#ifndef USE_VFORK
		/* Wait for the child process to configure the counters */
		sem_wait(sem_config_ready);
#endif
		profile_started=1;

		if ( (fd = pmct_open_monitor_entry())<0 )
			goto error_path;

		if (opts->kernel_buffer_size<4096) {
			/* Request shared memory region */
			if ((samples=pmct_request_shared_memory_region(fd,&max_buffer_samples))==NULL)
				goto error_path;
		} else {
			/* Reserve a big buffer from the heap directly */
			max_buffer_samples=opts->kernel_buffer_size/sizeof(pmc_sample_t);
			if ((samples=malloc(max_buffer_samples*sizeof(pmc_sample_t)))==NULL)
				goto error_path;
		}
		/* Print header if necessary */
		if (!(opts->flags & CMD_FLAG_ACUM_SAMPLES)) {
			print_counter_mappings(fo,opts,nr_experiments);
			pmct_print_header(fo,nr_experiments,pmcmask,virtual_mask,extended_output,0);
		}
		/* Print child counters */
		while(!stop_profiling) {
			/* Do this while !child_finished*/
			if (!child_finished) {
				alarm_ms(opts->msecs);
				pause();
			}

			/* Check if Ctrl+C was pressed */
			if(!stop_profiling) {
				nr_samples=pmct_read_samples(fd,samples,max_buffer_samples);

				if (nr_samples < 0)
					goto error_path;

				/* 
				 *  If we did not read anything and child already finished,
				 *  then exit loop 
				 */
				if (nr_samples==0 && child_finished)
					break;

				for (i=0; i<nr_samples; i++) {
					pmc_sample_t* cur=&samples[i];

					if (opts->flags & CMD_FLAG_ACUM_SAMPLES) {
						int j=0;
						unsigned char copy_metadata=0;

						/* Search PID in set */
						while (j<nr_pids && pid_ctrl_vector[j].pid!=cur->pid)
							j++;

						/* PID not found */
						if (j==nr_pids) {
							/* Add new item to set */
							pid_ctrl_vector[j].pid=cur->pid;
							pid_ctrl_vector[j].exp_mask=0;
							nr_pids++;
							acum_samples[j]=malloc(sizeof(pmc_sample_t)*nr_experiments);

							if (!acum_samples[j]) {
								fprintf(stderr,"Couldn't reserve memory for cummulative counters");
								goto error_path;
							}
						}

						if (! (pid_ctrl_vector[j].exp_mask & (1<<cur->exp_idx))) {
							/* Time to copy metadata ... */
							copy_metadata=1;
							pid_ctrl_vector[j].exp_mask|=1<<cur->exp_idx;
							pid_ctrl_vector[j].nr_samples_accum[cur->exp_idx]=1;
						} else
							pid_ctrl_vector[j].nr_samples_accum[cur->exp_idx]++;

						pmct_accumulate_sample (nr_experiments,pmcmask,virtual_mask,copy_metadata,cur,&acum_samples[j][cur->exp_idx]);
					} else {
						pmct_print_sample (fo,nr_experiments, pmcmask, virtual_mask, extended_output, cont, cur);
					}

					cont++;
					if ((opts->max_samples!=-1 && !child_finished && cont>opts->max_samples)
					    || (opts->timeout_secs!=-1 && !child_finished && check_timeout(opts->timeout_secs))) {
						kill(pid,SIGTERM);
						fprintf(stderr, "Maximum samples/timeout reached. Killing child process %d\n",pid);
						sleep(2);
					}
				}
			}
		}//end while

		/* Generate output from accumulated values */
		if (opts->flags & CMD_FLAG_ACUM_SAMPLES) {

			print_counter_mappings(fo,opts,nr_experiments);
			pmct_print_header(fo,nr_experiments,pmcmask,virtual_mask,extended_output,0);

			/* Generate samples for the various threads */
			for (i=0; i<nr_pids; i++) {
				int j=0;
				for (j=0; j<nr_experiments; j++) {
					if ( pid_ctrl_vector[i].exp_mask & (1<<j))
						pmct_print_sample (fo,nr_experiments, pmcmask, virtual_mask,
						                   extended_output, pid_ctrl_vector[i].nr_samples_accum[j], &acum_samples[i][j]);
				}
			}
		}

error_path:
		if (!child_finished) {
			wait4(pid,&child_status,0,&child_rusage);
			gettimeofday(&end_time, NULL);
		}
		if (opts->flags & CMD_FLAG_SHOW_CHILD_TIMES)
			print_process_statistics(fo,&child_rusage,&start_time,&end_time);

		if (fd>0)
			close(fd);
		exit(child_status);
	}//end parent code
}

#define MAX_CPUS 256

/* Config & Monitoring function for system-wide mode */
void monitoring_counters_syswide(struct options* opts,int optind,char** argv)
{
	unsigned int nr_virtual_counters=0,virtual_mask=0;
	int i, cont;
	unsigned int pmcmask=0;
	unsigned int npmcs = 0;
	int fd;
	struct timespec;
	pmc_sample_t* samples=NULL;
	pmc_sample_t** acum_samples=NULL;
	struct pid_ctrl* pid_ctrl_vector=NULL;
	int nr_pids=0;
	int nr_samples;
	unsigned int nr_experiments;
	unsigned int max_buffer_samples;

	cont = 1;
	fd = -1;
	child_status = 0;
	nr_virtual_counters=opts->nr_virtual_counters;
	virtual_mask=opts->virtual_mask;

	if (install_signal_handlers())
		exit(1);

	/* Gather info to build header */
	if (pmct_check_counter_config((const char**)opts->strcfg,&npmcs,&pmcmask,&ebs_on,&nr_experiments))
		exit(1);

	if (!opts->strcfg[0] && npmcs!=0)
		opts->flags |= CMD_FLAG_KERNEL_DRIVES_PMCS;

	if (nr_virtual_counters==0 && npmcs==0) {
		fprintf(stderr,"Please specify events to monitor!\n");
		exit(1);
	}

	if (opts->flags & CMD_FLAG_ACUM_SAMPLES) {
		acum_samples=malloc(sizeof(pmc_sample_t*)*MAX_CPUS);
		pid_ctrl_vector=malloc(sizeof(struct pid_ctrl)*MAX_CPUS); /* As much as 256 CPUs */
		if (!acum_samples || ! pid_ctrl_vector) {
			fprintf(stderr, "%s\n", "Couldn't reserve memory for cummulative counters");
			exit(1);
		}
		memset(acum_samples,0,sizeof(pmc_sample_t*)*MAX_CPUS);/* Set NULL POINTERS*/
	}

	/* In system-wide mode, the monitor program "owns" the counter configuration
		- So counter configuration must be done right here... before fork
	 */

	/* Set up kernel buffer size */
	if (opts->kernel_buffer_size!=-1 && pmct_set_kernel_buffer_size(opts->kernel_buffer_size))
		pmctrack_exit(1);

	/* Configure counters if there is something to configure */
	if (opts->strcfg[0] && pmct_config_counters((const char**)opts->strcfg,1))
		pmctrack_exit(1);

	/* Set up sampling period
		(check whether the kernel control the counters or not)
	*/
	if (pmct_config_timeout(opts->msecs,(!(opts->strcfg)[0] && npmcs!=0)))
		pmctrack_exit(1);

	if (opts->virtcfg && pmct_config_virtual_counters(opts->virtcfg,1))
		pmctrack_exit(1);

#ifndef USE_VFORK
	/* init semaphore to signal the parent that the child proccess has successfuly configured the counters */
	init_posix_semaphore(&sem_config_ready,0);
#endif
	/* Flag this stuff */
	profile_started=0;
	child_finished=0;
	stop_profiling = 0;

	/* Enable profiling !! */
	if (pmct_syswide_start_counting())
		pmctrack_exit(1);

	/* Keep track of start time */
	gettimeofday(&start_time, NULL);

#ifdef USE_VFORK
	pid = vfork();
#else
	pid = fork();
#endif
	if(pid == -1) {
		err(1,"Error forking process.");
	} else if(pid == 0) {
		// CHILD CODE:
		/* Bind first */
		bind_process_cpumask(getpid(),opts->cpumask);

#ifndef USE_VFORK
		/* Notify the parent that the configuration is done */
		sem_post(sem_config_ready);
#endif
		if(execvp(argv[optind], &argv[optind]) < 0) {
			warnx( "Error when trying to execute program %s\n", argv[optind] );
			pmctrack_exit(-1);
		}
	} else {	// PARENT CODE:

#ifndef USE_VFORK
		/* Wait for the child process to configure the counters */
		sem_wait(sem_config_ready);
#endif
		profile_started=1;

		if ( (fd = pmct_open_monitor_entry())<0 )
			goto error_path;

		if (opts->kernel_buffer_size<4096) {
			/* Request shared memory region */
			if ((samples=pmct_request_shared_memory_region(fd,&max_buffer_samples))==NULL)
				goto error_path;
		} else {
			/* Reserve a big buffer from the heap directly */
			max_buffer_samples=opts->kernel_buffer_size/sizeof(pmc_sample_t);
			if ((samples=malloc(max_buffer_samples*sizeof(pmc_sample_t)))==NULL)
				goto error_path;
		}
		/* Print header if necessary */
		if (!(opts->flags & CMD_FLAG_ACUM_SAMPLES)) {
			print_counter_mappings(fo,opts,nr_experiments);
			pmct_print_header(fo,nr_experiments,pmcmask,virtual_mask,extended_output,1);
		}

		/* Print child counters */
		while(!stop_profiling) {
			/* Do this while !child_finished*/
			if (!child_finished) {
				alarm_ms(opts->msecs);
				pause();
			}

			/* Check if Ctrl+C was pressed */
			if(!stop_profiling) {
				nr_samples=pmct_read_samples(fd,samples,max_buffer_samples);

				if (nr_samples < 0)
					goto error_path;

				/* If we did not read anything and child already finished,
				                 *  then exit loop */
				if (nr_samples==0 && child_finished)
					break;

				for (i=0; i<nr_samples; i++) {
					pmc_sample_t* cur=&samples[i];

					if (opts->flags & CMD_FLAG_ACUM_SAMPLES) {
						int j=0;
						unsigned char copy_metadata=0;

						/* Search PID in set */
						while (j<nr_pids && pid_ctrl_vector[j].pid!=cur->pid)
							j++;

						/* Not found */
						if (j==nr_pids) {
							/* Add new item to set */
							pid_ctrl_vector[j].pid=cur->pid;
							pid_ctrl_vector[j].exp_mask=0;
							nr_pids++;
							acum_samples[j]=malloc(sizeof(pmc_sample_t)*nr_experiments);

							if (!acum_samples[j]) {
								fprintf(stderr,"Couldn't reserve memory for cummulative counters");
								goto error_path;
							}
						}

						if (! (pid_ctrl_vector[j].exp_mask & (1<<cur->exp_idx))) {
							/* Time to copy metadata ... */
							copy_metadata=1;
							pid_ctrl_vector[j].exp_mask|=1<<cur->exp_idx;
							pid_ctrl_vector[j].nr_samples_accum[cur->exp_idx]=1;
						} else
							pid_ctrl_vector[j].nr_samples_accum[cur->exp_idx]++;

						pmct_accumulate_sample (nr_experiments,pmcmask,virtual_mask,copy_metadata,cur,&acum_samples[j][cur->exp_idx]);
					} else {
						pmct_print_sample (fo,nr_experiments, pmcmask, virtual_mask, extended_output, cont, cur);
					}

					cont++;
					if ((opts->max_samples!=-1 && !child_finished && cont>opts->max_samples)
					    || (opts->timeout_secs!=-1 && !child_finished && check_timeout(opts->timeout_secs))) {
						kill(pid,SIGTERM);
						fprintf(stderr, "Maximum samples/timeout reached. Killing child process %d\n",pid);
						sleep(2);
					}
				}
			}
		}//end while

		/* Generate output from accumulated values */
		if (opts->flags & CMD_FLAG_ACUM_SAMPLES) {

			print_counter_mappings(fo,opts,nr_experiments);
			pmct_print_header(fo,nr_experiments,pmcmask,virtual_mask,extended_output,1);

			/* Generate samples for the various threads */
			for (i=0; i<nr_pids; i++) {
				int j=0;
				for (j=0; j<nr_experiments; j++) {
					if ( pid_ctrl_vector[i].exp_mask & (1<<j))
						pmct_print_sample (fo,nr_experiments, pmcmask, virtual_mask,
						                   extended_output, pid_ctrl_vector[i].nr_samples_accum[j], &acum_samples[i][j]);
				}
			}
		}

error_path:
		if (!child_finished) {
			wait4(pid,&child_status,0,&child_rusage);
			gettimeofday(&end_time, NULL);
		}
		if (opts->flags & CMD_FLAG_SHOW_CHILD_TIMES)
			print_process_statistics(fo,&child_rusage,&start_time,&end_time);
		if (fd>0)
			close(fd);
		exit(child_status);
	}//end parent code
}

void sigchld_handler(int signo)
{

	/* Error since profile was not started when the child process finished */
	if (!profile_started)
		exit(1);

	if (wait4(pid,&child_status,WNOHANG,&child_rusage)>0) {
		gettimeofday(&end_time, NULL);
		child_finished=1;
	}
}

void sigint_handler(int signo)
{
	kill(SIGTERM, pid);
	/* Finish inmediately */
	stop_profiling=1;
	printf("Received signal %d\n", signo);
}


/* Give a default value for the various fields ... */
void init_options( struct options* opts)
{
	unsigned int i;

	opts->msecs = 1000;
	opts->virtcfg = NULL;
	opts->nr_virtual_counters=opts->virtual_mask=0;

	for (i = 0; i<MAX_RAW_COUNTER_CONFIGS_SAFE; ++i)
		opts->strcfg[i] = NULL;

	for (i = 0; i<MAX_COUNTER_CONFIGS; ++i)
		opts->user_cfg_str[i]=NULL;

	opts->cpumask = NO_CPU_BINDING;
	opts->max_samples = -1;
	opts->flags=0;
	opts->kernel_buffer_size = -1;
	opts->user_nr_configs=0;
	opts->pmu_id=0;
	memset(opts->event_mapping,0,sizeof(counter_mapping_t)*MAX_PERFORMANCE_COUNTERS);
	opts->global_pmcmask=0;
	opts->timeout_secs=-1; /* Disabled for now */
}


/* Insert the argument for an instance of the "-c" switch in a vector */
int add_cfg_string_to_options(char* usercfg, struct options* opts)
{
	char* cpstring;

	if (opts->user_nr_configs>=MAX_COUNTER_CONFIGS) {
		warnx("Sorry! cannot accept more PMC configuration strings");
		return 1;
	}

	/* Copy string as is */
	cpstring=malloc(strlen(usercfg)+1);
	/* Copy config */
	strcpy(cpstring,usercfg);
	opts->user_cfg_str[opts->user_nr_configs]=cpstring;
	opts->user_nr_configs++;

	return 0;
}

/* Wrapper for pmct_parse_pmc_configuration() */
int parse_pmc_configuration(struct options* opts)
{
	unsigned int nr_experiments;
	return pmct_parse_pmc_configuration((const char**)opts->user_cfg_str,
	                                    (opts->flags & CMD_FLAG_RAW_PMC_FORMAT),
	                                    opts->pmu_id,
	                                    opts->strcfg,
	                                    &nr_experiments,
	                                    &opts->global_pmcmask,
	                                    opts->event_mapping);
}


/* Wrapper for pmct_parse_vcounter_config() */
int set_vcount_string_in_options (char* user_vcount_string, struct options* opts)
{

	int mnemonics_used=0;

	if ( pmct_parse_vcounter_config(user_vcount_string,
	                                &opts->virtual_mask,
	                                &opts->nr_virtual_counters,
	                                &mnemonics_used,
	                                &opts->virtcfg)) {
		warnx("Error parsing virtual counter configuration\n");
		return 1;
	}

	if (mnemonics_used)
		opts->flags |= CMD_FLAG_VIRT_COUNTER_MNEMONICS;

	return 0;
}

/* Free up allocated memory for the various fields of struct options */
void free_options (struct options* opts)
{
	unsigned int i = 0;
	/* Free cfg string buffer */
	while (opts->strcfg[i]) {
		free(opts->strcfg[i]);
		++i;
	}

	i=0;
	while (opts->user_cfg_str[i]) {
		free(opts->user_cfg_str[i]);
		++i;
	}

	if (opts->virtcfg)
		free(opts->virtcfg);

	if (opts->event_mapping)
		free(opts->event_mapping);
}


static void usage(const char* program_name,int status)
{
	switch(status) {
	case 0:
		printf ("Usage: %s [OPTION [OP. ARGS]] [PROG [ARGS]]\n", program_name);
		printf ("Available oprions:");
		printf ("\n\t-c\t<config-string>\n\t\tset up a performance monitoring experiment using either raw or mnemonic-based PMC string");
		printf ("\n\t-o\t<output>\n\t\toutput: set output file for the results. (default = stdout.)");
		printf ("\n\t-T\t<Time>\n\t\tTime: elapsed time in seconds between two consecutive counter samplings. (default = 1 sec.)");
		printf ("\n\t-b\t<cpu or mask>\n\t\tbind launched program to the specified cpu o cpumask.");
		printf ("\n\t-n\t<max-samples>\n\t\tRun command until a given number of samples are collected");
		printf ("\n\t-N\t<secs>\n\t\tRun command for secs seconds only");
		printf ("\n\t-e\n\t\tEnable extended output");
		printf ("\n\t-A\n\t\tEnable aggregate count mode");
		printf ("\n\t-k\t<kernel_buffer_size>\n\t\tSpecify the size of the kernel buffer used for the PMC samples");
		printf ("\n\t-b\t<cpu or mask>\n\t\tbind monitor program to the specified cpu o cpumask.");
		printf ("\n\t-S\n\t\tEnable system-wide monitoring mode (per-CPU)");
		printf ("\n\t-r\t\n\t\tAccept pmc configuration strings in the RAW format");
		printf ("\n\t-p\t<pmu>\n\t\tSpecify the PMU id to use for the event configuration");
		printf ("\n\t-L\n\t\tLegacy-mode: do not show counter-to-event mapping");
		printf ("\n\t-t\n\t\tShow real, user and sys time of child process");
		printf ("\nPROG + ARGS:\n\t\tCommand line for the program to be monitored.\n");
		break;
	case -2:
		warnx("Usage: %s <prog> [program arguments]", program_name);
		break;
	case -3:
		warnx("Wrong format for PMC configuration string");
		break;
	case -4:
		warnx("Usage: %s -o <file>", program_name);
		break;
	default:
		warnx("Try `%s -h' to obtain more information.", program_name);
	}
	exit(status);
}

int main(int argc, char *argv[])
{
	fo = stdout;
	char optc;
	int system_wide=0;
	static struct options opts;

	init_options(&opts);

	if (argc==1)
		usage(argv[0],0);

	/* Process command-line options ... */
	while ((optc = getopt(argc, argv, "+hc:T:o:b:n:V:B:eAk:Srp:LtN:")) != (char)-1) {
		switch (optc) {
		case 'o':
			if((fo = fopen(optarg, "w")) == NULL)
				usage(argv[0],-4);
			break;
		case 'h':
			usage(argv[0],0);
			break;
		case 'c':
			if (add_cfg_string_to_options(optarg,&opts)<0)
				exit(1);
			break;
		case 'V':
			if (set_vcount_string_in_options(optarg,&opts) )
				exit(1);
			break;
		case 'T':
			opts.msecs = (int)1000.0*atof(optarg);
			break;
		case 'b':
			opts.cpumask=str_to_cpumask(optarg);
			break;
		case 'B':
			bind_process_cpumask(getpid(),str_to_cpumask(optarg));
			break;
		case 'n':
			opts.max_samples=atoi(optarg);
			break;
		case 'e':
			extended_output=1;
			opts.flags|=CMD_FLAG_EXTENDED_OUTPUT;
			break;
		case 'A':
			opts.flags|=CMD_FLAG_ACUM_SAMPLES;
			break;
		case 'k':
			opts.kernel_buffer_size=atoi(optarg);
			break;
		case 'S':
			system_wide=1;
			break;
		case 'r':
			opts.flags|=CMD_FLAG_RAW_PMC_FORMAT;
			break;
		case 'p':
			opts.pmu_id=atoi(optarg);
			break;
		case 'L':
			opts.flags|=CMD_FLAG_LEGACY_OUTPUT;
			break;
		case 't':
			opts.flags|=CMD_FLAG_SHOW_CHILD_TIMES;
			break;
		case 'N':
			opts.timeout_secs=atoi(optarg);
			break;
		default:
			fprintf(stderr, "Wrong option: %c\n", optc);
			exit(1);
		}
	}

	/* 
	 * Translate user-provided PMC configurations 
	 * into the raw format if necessary 
	 */
	if (parse_pmc_configuration(&opts))
		exit(1);

	if (argv[optind]!=NULL) {
		/* Get rid of stdio buffer */
		setbuf(fo,NULL);

		/* Invoke main monitoring function for the selected mode */
		if (system_wide)
			monitoring_counters_syswide(&opts,optind,argv);
		else
			monitoring_counters(&opts,optind,argv);
	}
	if(fo != stdout) fclose(fo);

	free_options(&opts);

	exit(EXIT_SUCCESS);
}