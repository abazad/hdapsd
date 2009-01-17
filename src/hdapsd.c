/*
 * hdapsd.c - Read from the HDAPS (HardDrive Active Protection System)
 *            and protect the drive if motion over threshold...
 *
 *            Derived from pivot.c by Robert Love.
 *
 * Copyright (C) 2005-2009 Jon Escombe <lists@dresco.co.uk>
 *                         Robert Love <rml@novell.com>
 *                         Shem Multinymous <multinymous@gmail.com>
 *                         Elias Oltmanns <eo@nebensachen.de>
 *                         Evgeni Golov <sargentd@die-welt.net>
 *
 * "Why does that kid keep dropping his laptop?"
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <getopt.h>
#include <linux/input.h>

#define PID_FILE                "/var/run/hdapsd.pid"
#define SYSFS_POSITION_FILE	    "/sys/devices/platform/hdaps/position"
#define MOUSE_ACTIVITY_FILE     "/sys/devices/platform/hdaps/keyboard_activity"
#define KEYBD_ACTIVITY_FILE     "/sys/devices/platform/hdaps/mouse_activity"
#define SAMPLING_RATE_FILE      "/sys/devices/platform/hdaps/sampling_rate"
#define POSITION_INPUTDEV       "/dev/input/hdaps/accelerometer-event"
#define BUF_LEN                 40

#define FREEZE_SECONDS          1    /* period to freeze disk */
#define REFREEZE_SECONDS        0.1  /* period after which to re-freeze disk */
#define FREEZE_EXTRA_SECONDS    4    /* additional timeout for kernel timer */
#define DEFAULT_SAMPLING_RATE   50   /* default sampling frequency */
#define SIGUSR1_SLEEP_SEC       8    /* how long to sleep upon SIGUSR1 */
/* Magic threshold tweak factors, determined experimentally to make a
 * threshold of 10-20 behave reasonably.
 */
#define VELOC_ADJUST            30.0
#define ACCEL_ADJUST            (VELOC_ADJUST * 60)
#define AVG_VELOC_ADJUST        3.0

/* History depth for velocity average, in seconds */
#define AVG_DEPTH_SEC           0.3

/* Parameters for adaptive threshold */
#define RECENT_PARK_SEC        3.0    /* How recent is "recently parked"? */
#define THRESH_ADAPT_SEC       1.0    /* How often to (potentially) change
                                       * the adaptive threshold?           */
#define THRESH_INCREASE_FACTOR 1.1    /* Increase factor when recently
                                       * parked but user is typing      */
#define THRESH_DECREASE_FACTOR 0.9985 /* Decrease factor when not recently
                                       * parked, per THRESH_ADAPT_SEC sec. */
#define NEAR_THRESH_FACTOR     0.8    /* Fraction of threshold considered
                                       * being near the threshold.       */

/* Threshold for *continued* parking, as fraction of normal threshold */
#define PARKED_THRESH_FACTOR   NEAR_THRESH_FACTOR /* >= NEAR_THRESH_FACTOR */

static int verbose = 0;
static int pause_now = 0;
static int dry_run = 0;
static int poll_sysfs = 0;
static int sampling_rate;

char pid_file[BUF_LEN] = "";
int hdaps_input_fd = 0;

struct list {
	char name[BUF_LEN];
	char protect_file[BUF_LEN];
	struct list *next;
};

struct list *disklist = NULL;

/*
 * slurp_file - read the content of a file (up to BUF_LEN-1) into a string.
 *
 * We open and close the file on every invocation, which is lame but due to
 * several features of sysfs files:
 *
 *	(a) Sysfs files are seekable.
 *	(b) Seeking to zero and then rereading does not seem to work.
 *
 * If I were king--and I will be one day--I would have made sysfs files
 * nonseekable and only able to return full-size reads.
 */
static int slurp_file(const char* filename, char* buf)
{
	int ret;
	int fd = open (filename, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", filename, strerror(errno));
		return fd;
	}	

	ret = read (fd, buf, BUF_LEN-1);
	if (ret < 0) {
		fprintf(stderr, "read(%s): %s\n", filename, strerror(errno));
	} else {
		buf[ret] = 0; /* null-terminate so we can parse safely */
	ret = 0;
	}

	if (close (fd))
		fprintf(stderr, "close(%s): %s\n", filename, strerror(errno));

	return ret;
}

/*
 * read_position_from_sysfs() - read the (x,y) position pair from hdaps via sysfs files
 * This method is not recommended for frequent polling, since it causes unnecessary interrupts
 * and a phase difference between hdaps-to-EC polling vs. hdapsd-to-hdaps polling.
 */
static int read_position_from_sysfs (int *x, int *y)
{
	char buf[BUF_LEN];
	int ret;
	if ((ret = slurp_file(SYSFS_POSITION_FILE, buf)))
		return ret;
	return (sscanf (buf, "(%d,%d)\n", x, y) != 2);
}

/*
 * read_int() - read an integer from a file
 */
static int read_int (const char* filename)
{
	char buf[BUF_LEN];
	int ret;
	if ((ret = slurp_file(filename, buf)))
		return ret;
	if (sscanf (buf, "%d\n", &ret) != 1)
		return -EIO;
	return ret;
}

/*
 * get_km_activity() - returns 1 if there is keyboard or mouse activity
 */
static int get_km_activity()
{
	if (read_int(MOUSE_ACTIVITY_FILE)==1)
		return 1;
	if (read_int(KEYBD_ACTIVITY_FILE)==1)
		return 1;
	return 0;
}


/*
 * read_position_from_inputdev() - read the (x,y) position pair and time from hdaps
 * via the hdaps input device. Blocks there is a change in position.
 * The x and y arguments should contain the last read values, since if one of them
 * doesn't change it will not be assigned.
 */
static int read_position_from_inputdev (int *x, int *y, double *utime)
{
	struct input_event ev;
	int len, done = 0;
	*utime = 0;
	while (1) {
		len = read(hdaps_input_fd, &ev, sizeof(struct input_event));
		if (len < 0) {
			fprintf(stderr, "ERROR: failed reading %s (%s).\n", POSITION_INPUTDEV, strerror(errno));
			return len;
		}
		if (len < (int)sizeof(struct input_event)) {
			fprintf(stderr, "ERROR: short read from %s (%d bytes).\n", POSITION_INPUTDEV, len);
			return -EIO;
		}
		switch (ev.type) {
			case EV_ABS: /* new X or Y */
				switch (ev.code) {
					case ABS_X:
						*x = ev.value;
						break;
					case ABS_Y:
						*y = ev.value; 
						break;
					default:
						continue;
				}
				break;
			case EV_SYN: /* X and Y now reflect latest measurement */
				done = 1;
				break;
			default:
				continue;
		}
		if (!*utime) /* first event's time is closest to reality */
			*utime = ev.time.tv_sec + ev.time.tv_usec/1000000.0;
		if (done)
			return 0;
	}
}


/*
 * write_protect() - park/unpark
 */
static int write_protect (const char *path, int val)
{
	int fd, ret;
	char buf[BUF_LEN];

	if (dry_run)
		return 0;

	snprintf(buf, BUF_LEN, "%d", val);

	fd = open (path, O_WRONLY);
	if (fd < 0) {
		perror ("open");
		return fd;
	}	

	ret = write (fd, buf, strlen(buf));

	if (ret < 0) {
		perror ("write");
		goto out;
	}
	ret = 0;

out:
	if (close (fd))
		perror ("close");

	return ret;
}

double get_utime (void)
{
	struct timeval tv;
	int ret = gettimeofday(&tv, NULL);
	if (ret) {
		perror("gettimeofday");
		exit(1);
	}
	return tv.tv_sec + tv.tv_usec/1000000.0;
}

/* Handler for SIGUSR1, sleeps for a few seconds. Useful when suspending laptop. */
void SIGUSR1_handler(int sig)
{
	signal(SIGUSR1, SIGUSR1_handler);
	pause_now=1;
}

/* Handler for SIGTERM, deletes the pidfile and exits. */
void SIGTERM_handler(int sig)
{
	signal(SIGTERM, SIGTERM_handler);
	unlink(pid_file);
	exit(0);
}

/*
 * usage() - display usage instructions and exit 
 */
void usage()
{
	printf("Usage: hdapsd [OPTIONS]\n");
	printf("\n");
	printf("Required options:\n");
	printf("   -d --device=<device>              <device> is likely to be hda or sda.\n");
	printf("                                     Can be given multiple times to protect multiple devices.\n");
	printf("   -s --sensitivity=<sensitivity>    A suggested starting <sensitivity> is 15.\n");
	printf("Additional options:\n");
	printf("   -a --adaptive                     Adaptive threshold (automatic\n");
	printf("                                     increase when the built-in\n");
	printf("                                     keyboard/mouse are used).\n");
	printf("   -v --verbose                      Get verbose statistics.\n");
	printf("   -b --background                   Run the process in the background.\n");
	printf("   -p --pidfile[=<pidfile>]          Create a pid file when running\n");
	printf("                                     in background.\n");
	printf("                                     If <pidfile> is not specified,\n");
	printf("                                     it's set to %s.\n", PID_FILE);
	printf("   -t --dry-run                      Don't actually park the drive.\n");
	printf("   -y --poll-sysfs                   Force use of sysfs interface to accelerometer.\n");
	printf("\n");
	printf("You can send SIGUSR1 to deactivate hdapsd for %d seconds.\n",
		SIGUSR1_SLEEP_SEC);
	printf("\n");
	exit(1);
}

/*
 * check_thresh() - compare a value to the threshold
 */
void check_thresh(double val_sqr, double thresh, int* above, int* near,
                 char* reason_out, char reason_mark)
{
	if (val_sqr > thresh*thresh*NEAR_THRESH_FACTOR*NEAR_THRESH_FACTOR) {
		*near = 1;
		*reason_out = tolower(reason_mark);
	}
	if (val_sqr > thresh*thresh) {
		*above = 1;
		*reason_out = toupper(reason_mark);
	}
}

/*
 * analyze() - make a decision on whether to park given present readouts
 *             (remembers some past data in local static variables).
 * Computes and checks 3 values:
 *   velocity:     current position - prev position / time delta
 *   acceleration: current velocity - prev velocity / time delta
 *   average velocity: exponentially decaying average of velocity,
 *                     weighed by time delta.
 * The velocity and acceleration tests respond quickly to short sharp shocks,
 * while the average velocity test catches long, smooth movements (and
 * averages out measurement noise).
 * The adaptive threshold, if enabled, increases when (built-in) keyboard or
 * mouse activity happens shortly after some value was above or near the
 * adaptive threshold. The adaptive threshold slowly decreases back to the
 * base threshold when no value approaches it.
 */

int analyze(int x, int y, double unow, double base_threshold, 
            int adaptive, int parked) 
{
	static int x_last = 0, y_last = 0;
	static double unow_last = 0, x_veloc_last = 0, y_veloc_last = 0;
	static double x_avg_veloc = 0, y_avg_veloc = 0;
	static int history = 0; /* how many recent valid samples? */
	static double adaptive_threshold = -1; /* current adaptive thresh */
	static int last_thresh_change = 0; /* last adaptive thresh change */
	static int last_near_thresh = 0; /* last time we were near thresh */
	static int last_km_activity; /* last time kbd/mouse activity seen */

	double udelta, x_delta, y_delta, x_veloc, y_veloc, x_accel, y_accel;
	double veloc_sqr, accel_sqr, avg_veloc_sqr;
	double exp_weight;
	double threshold; /* transient threshold for this iteration */
	char reason[4]; /* "which threshold reached?" string for verbose */
	int recently_near_thresh;
	int above=0, near=0; /* above threshold, near threshold */

	/* Adaptive threshold adjustment  */
	if (adaptive_threshold<0) /* first invocation */
		adaptive_threshold = base_threshold;
	recently_near_thresh = unow < last_near_thresh + RECENT_PARK_SEC;
 	if (adaptive && recently_near_thresh && get_km_activity())
		last_km_activity = unow;
	if (adaptive && unow > last_thresh_change + THRESH_ADAPT_SEC) {
		if (recently_near_thresh) {
			if (last_km_activity > last_near_thresh &&
			    last_km_activity > last_thresh_change) { 
				/* Near threshold and k/m activity */
				adaptive_threshold *= THRESH_INCREASE_FACTOR;
				last_thresh_change = unow;
			}
		} else {
			/* Recently never near threshold */
			adaptive_threshold *= THRESH_DECREASE_FACTOR;
			if (adaptive_threshold < base_threshold)
				adaptive_threshold = base_threshold;
			last_thresh_change = unow;
		}
	}
	
	/* compute deltas */
	udelta = unow - unow_last;
	x_delta = x - x_last;
	y_delta = y - y_last;

	/* compute velocity */
	x_veloc = x_delta/udelta;
	y_veloc = y_delta/udelta;
	veloc_sqr = x_veloc*x_veloc + y_veloc*y_veloc;

	/* compute acceleration */
	x_accel = (x_veloc - x_veloc_last)/udelta;
	y_accel = (y_veloc - y_veloc_last)/udelta;
	accel_sqr = x_accel*x_accel + y_accel*y_accel;

	/* compute exponentially-decaying velocity average */
	exp_weight = udelta/AVG_DEPTH_SEC; /* weight of this sample */
	exp_weight = 1 - 1.0/(1+exp_weight); /* softly clamped to 1 */
	x_avg_veloc = exp_weight*x_veloc + (1-exp_weight)*x_avg_veloc;
	y_avg_veloc = exp_weight*y_veloc + (1-exp_weight)*y_avg_veloc;
	avg_veloc_sqr = x_avg_veloc*x_avg_veloc + y_avg_veloc*y_avg_veloc;

	threshold = adaptive_threshold;
	if (parked) /* when parked, be reluctant to unpark */
		threshold *= PARKED_THRESH_FACTOR;

	/* Threshold test (uses Pythagoras's theorem) */
	strcpy(reason, "   ");
	
	check_thresh(veloc_sqr, threshold*VELOC_ADJUST,
	             &above, &near, reason+0, 'V');
	check_thresh(accel_sqr, threshold*ACCEL_ADJUST,
	             &above, &near, reason+1, 'A');
	check_thresh(avg_veloc_sqr, threshold*AVG_VELOC_ADJUST,
	             &above, &near, reason+2, 'X');

	if (verbose) {
		printf("dt=%5.3f  "
		       "dpos=(%3g,%3g)  "
		       "vel=(%6.1f,%6.1f)*%g  "
		       "acc=(%6.1f,%6.1f)*%g  "
		       "avg_vel=(%6.1f,%6.1f)*%g  "
		       "thr=%.1f  "
		       "%s\n",
		       udelta,
		       x_delta, y_delta,
		       x_veloc/VELOC_ADJUST,
		       y_veloc/VELOC_ADJUST,
		       VELOC_ADJUST*1.0,
		       x_accel/ACCEL_ADJUST,
		       y_accel/ACCEL_ADJUST,
		       ACCEL_ADJUST*1.0,
		       x_avg_veloc/AVG_VELOC_ADJUST,
		       y_avg_veloc/AVG_VELOC_ADJUST,
		       AVG_VELOC_ADJUST*1.0,
		       threshold,
		       reason);
	}

	if (udelta>1.0) { /* Too much time since last (resume from suspend?) */
		history = 0;
		x_avg_veloc = y_avg_veloc = 0;
	}

	if (history<2) { /* Not enough data for meaningful result */
		above = 0;
		near = 0;
		++history;
	}

	if (near)
		last_near_thresh = unow;

	x_last = x;
	y_last = y;
	x_veloc_last = x_veloc;
	y_veloc_last = y_veloc;
	unow_last = unow;

	return above;
}

/*
 * add_disk (disk) - add the given disk to the global disklist
 */
void add_disk (char* disk) {
	struct utsname sysinfo;
	char protect_file[BUF_LEN] = "";
	if (uname(&sysinfo) < 0 || strcmp("2.6.27", sysinfo.release) <= 0)
		snprintf(protect_file, BUF_LEN, "/sys/block/%s/device/unload_heads", disk);
	else
		snprintf(protect_file, BUF_LEN, "/sys/block/%s/queue/protect", disk);
	
	if (disklist == NULL) {
		disklist = (struct list *)malloc(sizeof(struct list));
		if (disklist == NULL) {
			printf("Error allocating memory\n");
			exit(EXIT_FAILURE);
		}
		else {
			strncpy(disklist->name,disk,BUF_LEN);
			strncpy(disklist->protect_file,protect_file,BUF_LEN);
			disklist->next = NULL;
		}
	}
	else {
		struct list *p = disklist;
		while (p->next != NULL)
			p = p->next;
		p->next = (struct list *)malloc(sizeof(struct list));
		if (p->next == NULL) {
			printf("Error allocating memory\n");
			exit(EXIT_FAILURE);
		}
		else {
			strncpy(p->next->name,disk,BUF_LEN);
			strncpy(p->next->protect_file,protect_file,BUF_LEN);
			p->next->next = NULL;
		}
	}
}

/*
 * free_disk (disk) - free the allocated memory
 */
void free_disk (struct list *disk) {
	if (disk != NULL) {
		if (disk->next != NULL)
			free_disk(disk->next);
		free(disk);
	}
}

/*
 * main() - loop forever, reading the hdaps values and 
 *          parking/unparking as necessary
 */
int main (int argc, char** argv)
{
	struct utsname sysinfo;
	int c, park_now, protect_factor;
	int x=0, y=0;
	int fd, i, ret, threshold = 0, background = 0, adaptive=0,
	  pidfile = 0, parked = 0;
	double unow = 0, parked_utime = 0;
	time_t now;

	if (uname(&sysinfo) < 0 || strcmp("2.6.27", sysinfo.release) <= 0)
		protect_factor = 1000;
	else
		protect_factor = 1;

	struct option longopts[] =
	{
		{"device", required_argument, NULL, 'd'},
		{"sensitivity", required_argument, NULL, 's'},
		{"adaptive", no_argument, NULL, 'a'},
		{"verbose", no_argument, NULL, 'v'},
		{"background", no_argument, NULL, 'b'},
		{"pidfile", optional_argument, NULL, 'p'},
		{"dry-run", no_argument, NULL, 't'},
		{"poll-sysfs", no_argument, NULL, 'y'},
		{NULL, 0, NULL, 0}
	};

	while ((c = getopt_long(argc, argv, "d:s:vbap::ty", longopts, NULL)) != -1) {
		switch (c) {
			case 'd':
				add_disk(optarg);
				break;
			case 's':
				threshold = atoi(optarg);
				break;
			case 'b':
				background = 1;
				break;
			case 'a':
				adaptive = 1;
				break;
			case 'v':
				verbose = 1;
				break;
			case 'p':
				pidfile = 1;
				if (optarg == NULL) {
					snprintf(pid_file, BUF_LEN, "%s", PID_FILE);
				} else {
					snprintf(pid_file, BUF_LEN, "%s", optarg);
				}
				break;
			case 't':
				printf("Dry run, will not actually park heads or freeze queue.\n");
				dry_run = 1;
				break;
			case 'y':
				poll_sysfs = 1;
				break;
			default:
				usage();
				break;
		}
	}
	
	if (!threshold || disklist == NULL)
		usage(argv);

	if (!poll_sysfs) {
		hdaps_input_fd = open(POSITION_INPUTDEV, O_RDONLY);
		if (hdaps_input_fd<0) {
			fprintf(stderr,
			        "WARNING: Cannot open hdaps position input file %s (%s). "
			        "You may be using an incompatible version of the hdaps module, "
			        "or missing the required udev rule. "
			        "Falling back to reading the position from sysfs (uses more power). "
			        "Use '-y' to silence this warning.\n",
			        POSITION_INPUTDEV, strerror(errno));
			poll_sysfs = 1;
		}
	}

	if (background) {
		verbose = 0;
		if (pidfile) {
			fd = open (pid_file, O_WRONLY | O_CREAT, 0644);
			if (fd < 0) {
				perror ("open(pid_file)");
				return 1;
			}
		}
		daemon(0,0);
		if (pidfile) {
			char buf[BUF_LEN];
			snprintf (buf, BUF_LEN, "%d\n", getpid());
			ret = write (fd, buf, strlen(buf));
			if (ret < 0) {
				perror ("write(pid_file)");
				return 1;
			}
			if (close (fd)) {
				perror ("close(pid_file)");
				return 1;
			}
		}
	}

	mlockall(MCL_FUTURE);

	if (verbose) {
		struct list *p = disklist;
		while (p != NULL) {
			printf("disk: %s\n", p->name);
			p = p->next;
		}
		printf("threshold: %i\n", threshold);
		printf("read_method: %s\n", poll_sysfs ? "poll-sysfs" : "input-dev");
	}

	/* check the protect attribute exists */
	/* wait for it if it's not there (in case the attribute hasn't been created yet) */
	struct list *p = disklist;
	while (p != NULL) {
		fd = open (p->protect_file, O_RDWR);
		if (background)
			for (i=0; fd < 0 && i < 100; ++i) {
				usleep (100000);	/* 10 Hz */
				fd = open (p->protect_file, O_RDWR);
			}
		if (fd < 0) {
			printf ("Could not open %s\n", p->protect_file);
			free_disk(disklist);
			return 1;
		}
		close (fd);
		p = p->next;
	}
	
	/* see if we can read the sensor */
	/* wait for it if it's not there (in case the attribute hasn't been created yet) */
	ret = read_position_from_sysfs (&x, &y);
	if (background)
		for (i=0; ret && i < 100; ++i) {
			usleep (100000);	/* 10 Hz */
			ret = read_position_from_sysfs (&x, &y);
		}
	if (ret)
		return 1;

    /* adapt to the driver's sampling rate */
	sampling_rate = read_int(SAMPLING_RATE_FILE);
	if (sampling_rate <= 0)
		sampling_rate = DEFAULT_SAMPLING_RATE;;
	if (verbose)
		printf("sampling_rate: %d\n", sampling_rate);

	signal(SIGUSR1, SIGUSR1_handler);

	if (background && pidfile) {
		signal(SIGTERM, SIGTERM_handler);
	}

	while (1) {
		if (poll_sysfs) {
			usleep (1000000/sampling_rate);
			ret = read_position_from_sysfs (&x, &y);
			unow = get_utime(); /* microsec */
		} else {
			double oldunow = unow;
			int oldx = x, oldy = y;
			ret = read_position_from_inputdev (&x, &y, &unow);

			/* The input device issues events only when the position changed.
			 * The analysis state needs to know how long the position remained
			 * unchanged, so send analyze() a fake retroactive update before sending
			 * the new one. */
			if (!ret && oldunow && unow-oldunow > 1.5/sampling_rate)
				analyze(oldx, oldy, unow-1.0/sampling_rate, threshold, adaptive, parked);
				
		}

		if (ret) {
			if (verbose)
				printf("readout error (%d)\n", ret);
			continue;
		}

		now = time((time_t *)NULL); /* sec */

		park_now = analyze(x, y, unow, threshold, adaptive, parked);

		if (park_now && !pause_now) {
			if (!parked || unow>parked_utime+REFREEZE_SECONDS) {
				/* Not frozen or freeze about to expire */
				struct list *p = disklist;
				while (p != NULL) {
					write_protect(p->protect_file,
					      (FREEZE_SECONDS+FREEZE_EXTRA_SECONDS) * protect_factor);
					p = p->next;
				}
				/* Write protect before any output (xterm, or 
				 * whatever else is handling our stdout, may be 
				 * swapped out).
				*/
				if (!parked)
					printf("%.24s: parking\n", ctime(&now));
				parked = 1;
				parked_utime = unow;
			} 
		} else {
			if (parked &&
			    (pause_now || unow>parked_utime+FREEZE_SECONDS)) {
				/* Sanity check */
				struct list *p = disklist;
				while (p != NULL) {
					if (!dry_run && !read_int(p->protect_file))
						printf("\nError! Not parked when we "
						       "thought we were... (paged out "
					               "and timer expired?)\n");
					/* Freeze has expired */
					write_protect(p->protect_file, 0); /* unprotect */
					p = p->next;
				}
				parked = 0;
				printf("%.24s: un-parking\n", ctime(&now));
			}
			while (pause_now) {
				pause_now=0;
				printf("%.24s: pausing for %d seconds\n", 
				       ctime(&now), SIGUSR1_SLEEP_SEC);
				sleep(SIGUSR1_SLEEP_SEC);
			}
		}

	}

	free_disk(disklist);
	munlockall();
	return ret;
}

