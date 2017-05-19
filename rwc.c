/*
 * rwc [-0dp] [PATH...] - report when changed
 *  -0  use NUL instead of newline for input/output separator
 *  -d  detect deletions too (prefixed with "- ")
 *  -p  pipe mode, don't generate new events if stdout pipe is not empty
 *
 * To the extent possible under law, Leah Neukirchen <leah@vuxu.org>
 * has waived all copyright and related or neighboring rights to this work.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

#include <sys/ioctl.h>
#ifdef __linux__
#include <sys/inotify.h>
#else
#include <sys/types.h>
#include <sys/event.h>
#define IN_DELETE 1
#define IN_DELETE_SELF 1
#endif

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *argv0;
char ibuf[8192];
int ifd;

int dflag;
int pflag;
char input_delim = '\n';

static void *root = 0; // tree

static int
order(const void *a, const void *b)
{
	return strcmp((char *)a, (char*)b);
}

#ifdef __linux__
struct wdmap {
        int wd;
        char *dir;
};

static void *wds = 0; // tree

static int
wdorder(const void *a, const void *b)
{
        struct wdmap *ia = (struct wdmap *)a;
        struct wdmap *ib = (struct wdmap *)b;

        if (ia->wd == ib->wd)
                return 0;
        else if (ia->wd < ib->wd)
                return -1;
        else
                return 1;
}

static void
add(char *file)
{
        struct stat st;
	int wd;

	char *dir = file;

	tsearch(strdup(file), &root, order);

	// assume non-existing files are regular files
	if (lstat(file, &st) < 0 || !S_ISDIR(st.st_mode))
		dir = dirname(file);

	wd = inotify_add_watch(ifd, dir, IN_MOVED_TO | IN_CLOSE_WRITE | dflag);
	if (wd < 0) {
		fprintf(stderr, "%s: inotify_add_watch: %s: %s\n",
			argv0, dir, strerror(errno));
	} else {
		struct wdmap *newkey = malloc(sizeof (struct wdmap));
		newkey->wd = wd;
		newkey->dir = dir;
		tsearch(newkey, &wds, wdorder);
	}
}

static void
run(void)
{
	while (1) {
		ssize_t len, i;
		struct inotify_event *ev;

		len = read(ifd, ibuf, sizeof ibuf);
		if (len <= 0) {
			fprintf(stderr, "%s: error reading inotify buffer: %s",
			    argv0, strerror(errno));
			exit(1);
		}
	
		for (i = 0; i < len; i += sizeof (*ev) + ev->len) {
			ev = (struct inotify_event *) (ibuf + i);

			if (ev->mask & IN_IGNORED)
				continue;

			struct wdmap key, **result;
			key.wd = ev->wd;
			key.dir = 0;
			result = tfind(&key, &wds, wdorder);
			if (!result)
				continue;

			char *dir = (*result)->dir;
			char fullpath[PATH_MAX];
			char *name = ev->name;
			if (strcmp(dir, ".") != 0) {
				snprintf(fullpath, sizeof fullpath, "%s/%s",
					 dir, ev->name);
				name = fullpath;
			}

			if (tfind(name, &root, order) ||
			    tfind(dir, &root, order)) {
				if (pflag) {
					int n;
					ioctl(1, FIONREAD, &n);
					if (n > 0)
						break;
				}
				printf("%s%s%c",
				    (ev->mask & IN_DELETE ? "- " : ""),
				    name,
				    input_delim);
				fflush(stdout);
			}
		}
	}
}
#else
static void
add(char *file)
{
	struct kevent ev;
        struct stat st;
	int wd;
	char *key;

	key = strdup(file);

	// check if the file is already registered
	if (tfind(key, &root, order))
		return;

	// assume non-existing files are regular files
	if (lstat(file, &st) && S_ISDIR(st.st_mode)) {
		//
	}

	wd = open(file, O_RDONLY | O_NONBLOCK);
	if (wd < 0) {
		fprintf(stderr, "%s: open: %s: %s\n",
			argv0, file, strerror(errno));
	} else {
		// (S_ISDIR(st.st_mode) ? NOTE_EXTEND : 0) | NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME | NOTE_TRUNCATE | (dflag ? NOTE_DELETE : 0),
		EV_SET(&ev, wd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
		    NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME | (dflag ? NOTE_DELETE : 0),
		    0, file);
	if (kevent(ifd, &ev, 1, 0, 0, 0) < 0)
		fprintf(stderr, "%s: kevent: %s: %s\n",
			argv0, file, strerror(errno));
	tsearch(key, &root, order);
}
}

static void
run(void)
{
struct kevent eventlist[5];
struct kevent *ev;
int len, i;

while (1) {
	len = kevent(ifd, 0, 0, eventlist, 5, 0);
	for (i = 0; i < len; i++) {
		ev = &eventlist[i];
		if (ev->flags & EV_ERROR) {
			fprintf(stderr, "EV_ERROR\n");
			exit(111);
		}

			if (pflag) {
				int n;
				ioctl(1, FIONREAD, &n);
				if (n > 0)
					break;
			}
			printf("%s%s%c",
			    (ev->fflags & NOTE_DELETE ? "- " : ""),
			    (char *)ev->udata,
			    input_delim);
			fflush(stdout);
		}
	}
}
#endif

int
main(int argc, char *argv[])
{
	int c, i;
	char *line = 0;

	argv0 = argv[0];

        while ((c = getopt(argc, argv, "0dp")) != -1)
		switch(c) {
		case '0': input_delim = 0; break;
		case 'd': dflag = IN_DELETE | IN_DELETE_SELF; break;
		case 'p': pflag++; break;
		default:
                        fprintf(stderr, "Usage: %s [-0d] [PATH...]\n", argv0);
                        exit(2);
                }

#ifdef __linux__
        ifd = inotify_init();
        if (ifd < 0) {
		fprintf(stderr, "%s: inotify_init: %s\n",
		    argv0, strerror(errno));
                exit(111);
	}
#else
	ifd = kqueue();
        if (ifd < 0) {
		fprintf(stderr, "%s: kqueue: %s\n",
		    argv0, strerror(errno));
                exit(111);
	}
#endif

	i = optind;
	if (optind == argc)
		goto from_stdin;
	for (; i < argc; i++) {
		if (strcmp(argv[i], "-") != 0) {
			add(argv[i]);
			continue;
		}
from_stdin:
		while (1) {
			size_t linelen = 0;
			ssize_t rd;

			errno = 0;
			rd = getdelim(&line, &linelen, input_delim, stdin);
			if (rd == -1) {
				if (errno != 0)
					return -1;
				break;
			}
			
			if (rd > 0 && line[rd-1] == input_delim)
				line[rd-1] = 0;  // strip delimiter

			add(line);
		}
	}
	free(line);

	run();

        return 0;
}
