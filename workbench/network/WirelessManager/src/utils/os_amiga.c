/*
 * wpa_supplicant/hostapd / OS specific functions for Amiga-like systems
 * Copyright (c) 2005-2019, Jouni Malinen <j@w1.fi>
 * Copyright (c) 2010-2026, Neil Cafferkey
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

/* setenv()/unsetenv() are POSIX.1-2001 in the AROS headers */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "includes.h"

#include <time.h>
#include <sys/stat.h>

#include "os.h"
#include "common.h"

#include <dos/dos.h>
#include <devices/timer.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>


void os_sleep(os_time_t sec, os_time_t usec)
{
	ULONG ticks = sec * TICKS_PER_SECOND +
		(usec + (1000000 / TICKS_PER_SECOND) - 1) /
		(1000000 / TICKS_PER_SECOND);

	Delay(ticks);
}


int os_get_time(struct os_time *t)
{
	struct timeval tv;

	if (gettimeofday(&tv, NULL) != 0)
		return -1;
	t->sec = tv.tv_sec;
	t->usec = tv.tv_usec;
	return 0;
}


int os_get_reltime(struct os_reltime *t)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		t->sec = ts.tv_sec;
		t->usec = ts.tv_nsec / 1000;
		return 0;
	}

	/* Fall back to wall-clock time */
	{
		struct os_time now;
		int res = os_get_time(&now);

		if (res == 0) {
			t->sec = now.sec;
			t->usec = now.usec;
		}
		return res;
	}
}


int os_mktime(int year, int month, int day, int hour, int min, int sec,
	      os_time_t *t)
{
	struct tm tm, *tm1;
	time_t t_local, t1, t2;
	os_time_t tz_offset;

	if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
	    hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 ||
	    sec > 60)
		return -1;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = year - 1900;
	tm.tm_mon = month - 1;
	tm.tm_mday = day;
	tm.tm_hour = hour;
	tm.tm_min = min;
	tm.tm_sec = sec;

	t_local = mktime(&tm);

	/* figure out offset to UTC */
	tm1 = localtime(&t_local);
	if (tm1) {
		t1 = mktime(tm1);
		tm1 = gmtime(&t_local);
		if (tm1) {
			t2 = mktime(tm1);
			tz_offset = t2 - t1;
		} else
			tz_offset = 0;
	} else
		tz_offset = 0;

	*t = (os_time_t) t_local - tz_offset;
	return 0;
}


int os_gmtime(os_time_t t, struct os_tm *tm)
{
	struct tm *tm2;
	time_t t2 = t;

	tm2 = gmtime(&t2);
	if (tm2 == NULL)
		return -1;
	tm->sec = tm2->tm_sec;
	tm->min = tm2->tm_min;
	tm->hour = tm2->tm_hour;
	tm->day = tm2->tm_mday;
	tm->month = tm2->tm_mon + 1;
	tm->year = tm2->tm_year + 1900;
	return 0;
}


int os_daemonize(const char *pid_file)
{
	return -1;
}


void os_daemonize_terminate(const char *pid_file)
{
	if (pid_file)
		unlink(pid_file);
}


int os_get_random(unsigned char *buf, size_t len)
{
	/* SAE, nonces and the internal TLS all draw on this, so it has to be
	 * the C library's entropy source, not rand(). getentropy() caps a single
	 * request at 256 bytes. */
	while (len > 0) {
		size_t chunk = len > 256 ? 256 : len;

		if (getentropy(buf, chunk) != 0) {
			arc4random_buf(buf, chunk);
		}
		buf += chunk;
		len -= chunk;
	}

	return 0;
}


unsigned long os_random(void)
{
	unsigned long val;

	os_get_random((unsigned char *) &val, sizeof(unsigned long));

	return val;
}


char * os_rel2abs_path(const char *rel_path)
{
	char *buf = NULL, *cwd, *ret;
	size_t len = 128, cwd_len, rel_len, ret_len;
	int last_errno;

	if (!rel_path)
		return NULL;

	/* "VOLUME:path" is already absolute */
	if (strchr(rel_path, ':') != NULL)
		return os_strdup(rel_path);

	for (;;) {
		buf = os_malloc(len);
		if (buf == NULL)
			return NULL;
		cwd = getcwd(buf, len);
		if (cwd == NULL) {
			last_errno = errno;
			os_free(buf);
			if (last_errno != ERANGE)
				return NULL;
			len *= 2;
			if (len > 2000)
				return NULL;
		} else {
			buf[len - 1] = '\0';
			break;
		}
	}

	cwd_len = os_strlen(cwd);
	rel_len = os_strlen(rel_path);
	ret_len = cwd_len + 1 + rel_len + 1;
	ret = os_malloc(ret_len);
	if (ret) {
		os_memcpy(ret, cwd, cwd_len);
		if (cwd_len > 0 && ret[cwd_len - 1] == ':') {
			os_memcpy(ret + cwd_len, rel_path, rel_len);
			ret[cwd_len + rel_len] = '\0';
		} else {
			ret[cwd_len] = '/';
			os_memcpy(ret + cwd_len + 1, rel_path, rel_len);
			ret[ret_len - 1] = '\0';
		}
	}
	os_free(buf);
	return ret;
}


int os_program_init(void)
{
	return 0;
}


void os_program_deinit(void)
{
}


int os_setenv(const char *name, const char *value, int overwrite)
{
	return setenv(name, value, overwrite);
}


int os_unsetenv(const char *name)
{
	return unsetenv(name);
}


char * os_readfile(const char *name, size_t *len)
{
	FILE *f;
	char *buf;
	long pos;

	f = fopen(name, "rb");
	if (f == NULL)
		return NULL;

	if (fseek(f, 0, SEEK_END) < 0 || (pos = ftell(f)) < 0) {
		fclose(f);
		return NULL;
	}
	*len = pos;
	if (fseek(f, 0, SEEK_SET) < 0) {
		fclose(f);
		return NULL;
	}

	buf = os_malloc(*len);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}

	if (fread(buf, 1, *len, f) != *len) {
		fclose(f);
		os_free(buf);
		return NULL;
	}

	fclose(f);

	return buf;
}


int os_file_exists(const char *fname)
{
	return access(fname, F_OK) == 0;
}


int os_fdatasync(FILE *stream)
{
	if (!fflush(stream))
		return fsync(fileno(stream));
	return -1;
}


void * os_zalloc(size_t size)
{
	return calloc(1, size);
}


size_t os_strlcpy(char *dest, const char *src, size_t siz)
{
	const char *s = src;
	size_t left = siz;

	if (left) {
		/* Copy string up to the maximum size of the dest buffer */
		while (--left != 0) {
			if ((*dest++ = *s++) == '\0')
				break;
		}
	}

	if (left == 0) {
		/* Not enough room for the string; force NUL-termination */
		if (siz != 0)
			*dest = '\0';
		while (*s++)
			; /* determine total src string length */
	}

	return s - src - 1;
}


int os_memcmp_const(const void *a, const void *b, size_t len)
{
	const u8 *aa = a;
	const u8 *bb = b;
	size_t i;
	u8 res;

	for (res = 0, i = 0; i < len; i++)
		res |= aa[i] ^ bb[i];

	return res;
}


void * os_memdup(const void *src, size_t len)
{
	void *r = os_malloc(len);

	if (r && src)
		os_memcpy(r, src, len);
	return r;
}


int os_exec(const char *program, const char *arg, int wait_completion)
{
	return -1;
}


/* inet(3) without bsdsocket.library: dotted-quad IPv4 only */

char * inet_ntoa(struct in_addr in)
{
	static char buf[16];
	const unsigned char *p = (const unsigned char *) &in.s_addr;

	snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
	return buf;
}


int inet_aton(const char *cp, struct in_addr *addr)
{
	unsigned int part[4];
	unsigned char *out;
	int i;
	const char *p = cp;

	for (i = 0; i < 4; i++) {
		char *end;
		unsigned long v;

		if (!isdigit((unsigned char) *p))
			return 0;
		v = strtoul(p, &end, 10);
		if (v > 255)
			return 0;
		part[i] = v;
		p = end;
		if (i < 3) {
			if (*p != '.')
				return 0;
			p++;
		}
	}
	if (*p != '\0')
		return 0;

	if (addr) {
		out = (unsigned char *) &addr->s_addr;
		for (i = 0; i < 4; i++)
			out[i] = part[i];
	}
	return 1;
}


const char * inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
	const unsigned char *p = src;
	char buf[16];

	if (af != AF_INET) {
		errno = EAFNOSUPPORT;
		return NULL;
	}
	snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
	if (os_strlen(buf) >= size) {
		errno = ENOSPC;
		return NULL;
	}
	os_strlcpy(dst, buf, size);
	return dst;
}


int inet_pton(int af, const char *src, void *dst)
{
	if (af != AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	return inet_aton(src, dst) ? 1 : 0;
}

/* The connection state for other programs: ENV:WirelessManager/State holds
 * wpa_supplicant's state name, written on every change. */
void aros_publish_state(const char *state)
{
	SetVar((CONST_STRPTR)"WirelessManager/State", (CONST_STRPTR)state, -1,
	       GVF_GLOBAL_ONLY | LV_VAR);
}
