/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2012 Whamcloud, Inc.
 */
/*
 * lustre/utils/lustre_lfsck.c
 *
 * Lustre user-space tools for LFSCK.
 *
 * Author: Fan Yong <yong.fan@whamcloud.com>
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>

#include "obdctl.h"

#include <obd.h>
#include <lustre/lustre_lfsck_user.h>
#include <libcfs/libcfsutil.h>
#include <lnet/lnetctl.h>

static struct option long_opt_start[] = {
	{"device",      required_argument, 0, 'M'},
	{"error",       required_argument, 0, 'e'},
	{"help",	no_argument,       0, 'h'},
	{"dryrun",      required_argument, 0, 'n'},
	{"reset",       no_argument,       0, 'r'},
	{"speed",       required_argument, 0, 's'},
	{"type",	required_argument, 0, 't'},
	{0,		0,		   0,   0}
};

static struct option long_opt_stop[] = {
	{"device",      required_argument, 0, 'M'},
	{"help",	no_argument,       0, 'h'},
	{0,		0,		   0,   0}
};

struct lfsck_types_names {
	char   *name;
	__u16   type;
};

static struct lfsck_types_names lfsck_types_names[3] = {
	{ "layout",     LT_LAYOUT },
	{ "DNE",	LT_DNE },
	{ 0,		0 }
};

static void usage_start(void)
{
	fprintf(stderr, "Start LFSCK.\n"
		"SYNOPSIS:\n"
		"lfsck_start <-M | --device MDT_device>\n"
		"	     [-e | --error error_handle] [-h | --help]\n"
		"	     [-n | --dryrun switch] [-r | --reset]\n"
		"	     [-s | --speed speed_limit]\n"
		"	     [-t | --type lfsck_type[,lfsck_type...]]\n"
		"OPTIONS:\n"
		"-M: The MDT device to start LFSCK on.\n"
		"-e: Error handle, 'continue'(default) or 'abort'.\n"
		"-h: Help information.\n"
		"-n: Check without modification. 'off'(default) or 'on'.\n"
		"-r: Reset scanning start position to the device beginning.\n"
		"-s: How many items can be scanned at most per second. "
		    "'%d' means no limit (default).\n"
		"-t: The LFSCK type(s) to be started.\n",
		LFSCK_SPEED_NO_LIMIT);
}

static void usage_stop(void)
{
	fprintf(stderr, "Stop LFSCK.\n"
		"SYNOPSIS:\n"
		"lfsck_stop <-M | --device MDT_device> [-h | --help]\n"
		"OPTIONS:\n"
		"-M: The MDT device to stop LFSCK on.\n"
		"-h: Help information.\n");
}

static int lfsck_pack_dev(struct obd_ioctl_data *data, char *arg)
{
	data->ioc_inllen4 = strlen(arg) + 1;
	if (data->ioc_inllen4 > MAX_OBD_NAME) {
		fprintf(stderr, "MDT device name is too long. "
			"Valid length should be less than %d\n", MAX_OBD_NAME);
		return -EINVAL;
	}

	data->ioc_inlbuf4 = arg;
	data->ioc_dev = OBD_DEV_BY_DEVNAME;
	return 0;
}

int jt_lfsck_start(int argc, char **argv)
{
	struct obd_ioctl_data data;
	char rawbuf[MAX_IOC_BUFLEN], *buf = rawbuf;
	char device[MAX_OBD_NAME];
	struct lfsck_start start;
	char *optstring = "M:e:hi:n:rs:t:";
	int opt, index, rc, val, i;

	memset(&data, 0, sizeof(data));
	memset(&start, 0, sizeof(start));
	start.ls_version = LFSCK_VERSION_V1;
	start.ls_active = LFSCK_TYPES_DEF;
	while ((opt = getopt_long(argc, argv, optstring, long_opt_start,
				  &index)) != EOF) {
		switch (opt) {
		case 'M':
			rc = lfsck_pack_dev(&data, optarg);
			if (rc != 0)
				return rc;
			break;
		case 'e':
			if (strcmp(optarg, "abort") == 0) {
				start.ls_flags |= LPF_FAILOUT;
			} else if (strcmp(optarg, "continue") != 0) {
				fprintf(stderr, "Invalid error handler: %s. "
					"The valid value should be: 'continue'"
					"(default) or 'abort'.\n", optarg);
				return -EINVAL;
			}
			start.ls_valid |= LSV_ERROR_HANDLE;
			break;
		case 'h':
			usage_start();
			return 0;
		case 'n':
			if (strcmp(optarg, "on") == 0) {
				start.ls_flags |= LPF_DRYRUN;
			} else if (strcmp(optarg, "off") != 0) {
				fprintf(stderr, "Invalid dryrun switch: %s. "
					"The valid value shou be: 'off'"
					"(default) or 'on'\n", optarg);
				return -EINVAL;
			}
			start.ls_valid |= LSV_DRYRUN;
			break;
		case 'r':
			start.ls_flags |= LPF_RESET;
			break;
		case 's':
			val = atoi(optarg);
			start.ls_speed_limit = val;
			start.ls_valid |= LSV_SPEED_LIMIT;
			break;
		case 't': {
			char *str = optarg, *p, c;

			start.ls_active = 0;
			while (*str) {
				while (*str == ' ' || *str == ',')
					str++;

				if (*str == 0)
					break;

				p = str;
				while (*p != 0 && *p != ' ' && *p != ',')
					p++;

				c = *p;
				*p = 0;
				for (i = 0; i < 3; i++) {
					if (strcmp(str,
						   lfsck_types_names[i].name)
						   == 0) {
						start.ls_active |=
						    lfsck_types_names[i].type;
						break;
					}
				}
				*p = c;
				str = p;

				if (i >= 3 ) {
					fprintf(stderr, "Invalid LFSCK type.\n"
						"The valid value should be "
						"'layout' or 'DNE'.\n");
					return -EINVAL;
				}
			}
			if (start.ls_active == 0) {
				fprintf(stderr, "Miss LFSCK type(s).\n"
					"The valid value should be "
					"'layout' or 'DNE'.\n");
				return -EINVAL;
			}
			break;
		}
		default:
			fprintf(stderr, "Invalid option, '-h' for help.\n");
			return -EINVAL;
		}
	}

	if (data.ioc_inlbuf4 == NULL) {
		fprintf(stderr,
			"Must sepcify MDT device to start LFSCK.\n");
		return -EINVAL;
	}

	memset(device, 0, MAX_OBD_NAME);
	memcpy(device, data.ioc_inlbuf4, data.ioc_inllen4);
	data.ioc_inlbuf4 = device;
	data.ioc_inlbuf1 = (char *)&start;
	data.ioc_inllen1 = sizeof(start);
	memset(buf, 0, sizeof(rawbuf));
	rc = obd_ioctl_pack(&data, &buf, sizeof(rawbuf));
	if (rc) {
		fprintf(stderr, "Fail to pack ioctl data: rc = %d.\n", rc);
		return rc;
	}

	rc = l_ioctl(OBD_DEV_ID, OBD_IOC_START_LFSCK, buf);
	if (rc < 0) {
		perror("Fail to start LFSCK");
		return rc;
	}

	obd_ioctl_unpack(&data, buf, sizeof(rawbuf));
	printf("Started LFSCK on the MDT device %s:", device);
	if (start.ls_active == 0) {
		printf(" noop");
	} else {
		for (i = 0; i < 2; i++) {
			if (start.ls_active & lfsck_types_names[i].type) {
				printf(" %s", lfsck_types_names[i].name);
				start.ls_active &= ~lfsck_types_names[i].type;
			}
		}
		if (start.ls_active != 0)
			printf(" unknown(0x%x)", start.ls_active);
	}
	printf("\n");
	return 0;
}

int jt_lfsck_stop(int argc, char **argv)
{
	struct obd_ioctl_data data;
	char rawbuf[MAX_IOC_BUFLEN], *buf = rawbuf;
	char device[MAX_OBD_NAME];
	char *optstring = "M:h";
	int opt, index, rc;

	memset(&data, 0, sizeof(data));
	while ((opt = getopt_long(argc, argv, optstring, long_opt_stop,
				  &index)) != EOF) {
		switch (opt) {
		case 'M':
			rc = lfsck_pack_dev(&data, optarg);
			if (rc != 0)
				return rc;
			break;
		case 'h':
			usage_stop();
			return 0;
		default:
			fprintf(stderr, "Invalid option, '-h' for help.\n");
			return -EINVAL;
		}
	}

	if (data.ioc_inlbuf4 == NULL) {
		fprintf(stderr,
			"Must sepcify MDT device to stop LFSCK.\n");
		return -EINVAL;
	}

	memset(device, 0, MAX_OBD_NAME);
	memcpy(device, data.ioc_inlbuf4, data.ioc_inllen4);
	data.ioc_inlbuf4 = device;
	memset(buf, 0, sizeof(rawbuf));
	rc = obd_ioctl_pack(&data, &buf, sizeof(rawbuf));
	if (rc) {
		fprintf(stderr, "Fail to pack ioctl data: rc = %d.\n", rc);
		return rc;
	}

	rc = l_ioctl(OBD_DEV_ID, OBD_IOC_STOP_LFSCK, buf);
	if (rc < 0) {
		perror("Fail to stop LFSCK");
		return rc;
	}

	printf("Stopped LFSCK on the MDT device %s.\n", device);
	return 0;
}