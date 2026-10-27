/* Linux-only benchmark observer. Fork after a fresh exec so the measured
 * worker cannot inherit the large oracle controller's pre-exec RSS peak. */
#define _GNU_SOURCE
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 5) return 125;
    pid_t child = fork();
    if (child < 0) return 125;
    if (!child) {
        struct rlimit as = {strtoull(argv[2], NULL, 10), strtoull(argv[2], NULL, 10)};
        struct rlimit file = {strtoull(argv[3], NULL, 10), strtoull(argv[3], NULL, 10)};
        if (setrlimit(RLIMIT_AS, &as) || setrlimit(RLIMIT_FSIZE, &file)) _exit(125);
        execvp(argv[4], argv + 4);
        _exit(127);
    }
    int status;
    struct rusage usage;
    while (wait4(child, &status, 0, &usage) < 0) if (errno != EINTR) return 125;
    FILE *out = fopen(argv[1], "w");
    if (!out) return 125;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
    fprintf(out, "{\"returncode\":%d,\"process_peak_rss_kib\":%ld,"
        "\"process_user_cpu_seconds\":%.9f,\"process_system_cpu_seconds\":%.9f,"
        "\"process_input_blocks\":%ld,\"process_output_blocks\":%ld}\n",
        code, usage.ru_maxrss, usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6,
        usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6, usage.ru_inblock, usage.ru_oublock);
    if (fclose(out)) return 125;
    return code < 0 ? 128 - code : code;
}
