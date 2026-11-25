// Linux-only syscall interposer. Armed by the test driver after transaction
// staging. Labels distinguish the WAL and main's temporary file and namespace.

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char kind = '?', last = '?';
static const char *action(char k, const char *name) {
	const char *env = getenv("ZYX_TEST_NATIVE_IO");
	char event[100];
	snprintf(event, sizeof(event), "%c.%s=", k, name);
	return env && !strncmp(env, event, strlen(event)) ? env + strlen(event) : "";
}
static int fault(char k, const char *name) {
	if (getenv("ZYX_TEST_NATIVE_IO") && !strcmp(getenv("ZYX_TEST_NATIVE_IO"), "trace"))
		fprintf(stderr, "IO %c.%s\n", k, name);
	const char *mode = action(k, name);
	if (!strcmp(mode, "kill"))
		kill(getpid(), SIGKILL);
	if (!strcmp(mode, "eio")) {
		errno = EIO;
		return -1;
	}
	return 0;
}
ssize_t write(int fd, const void *data, size_t len) {
	static ssize_t (*real)(int, const void *, size_t);
	if (!real)
		real = reinterpret_cast<decltype(real)>(dlsym(RTLD_NEXT, "write"));
	if (len >= 8 && (!memcmp(data, "ZYXng001", 8) || !memcmp(data, "ZYXnw001", 8))) {
		kind = ((const char *) data)[4] == 'g' ? 'm' : 'w';

		if (fault(kind, "write") < 0)
			return -1;
		if (!strcmp(action(kind, "write"), "short"))
			len = 1;
		if (!strcmp(action(kind, "write"), "zero"))
			return 0;
	}
	return real(fd, data, len);
}
int fsync(int fd) {
	static int (*real)(int);
	if (!real)
		real = reinterpret_cast<decltype(real)>(dlsym(RTLD_NEXT, "fsync"));
	struct stat st;
	if (fstat(fd, &st) < 0)
		return -1;
	int dir = S_ISDIR(st.st_mode);
	char k = dir ? last : kind;
	const char *name = dir ? "dirsync" : "sync";
	if (fault(k, name) < 0)
		return -1;
	int ret = real(fd);
	if (ret == 0)
		fault(k, dir ? "dirsynced" : "synced");
	return ret;
}
int renameat(int a, const char *old, int b, const char *newname) {
	static int (*real)(int, const char *, int, const char *);
	if (!real)
		real = reinterpret_cast<decltype(real)>(dlsym(RTLD_NEXT, "renameat"));
	char k = strstr(newname, ".wal") ? 'w' : 'm';
	if (fault(k, "rename") < 0)
		return -1;
	int ret = real(a, old, b, newname);
	if (!ret) {
		last = k;
		fault(k, "renamed");
	}
	return ret;
}
int unlinkat(int fd, const char *name, int flags) {
	static int (*real)(int, const char *, int);
	if (!real)
		real = reinterpret_cast<decltype(real)>(dlsym(RTLD_NEXT, "unlinkat"));
	if (strstr(name, ".wal") && fault('w', "unlink") < 0)
		return -1;
	int ret = real(fd, name, flags);
	if (!ret && strstr(name, ".wal")) {
		last = 'u';
		fault('w', "unlinked");
	}
	return ret;
}
