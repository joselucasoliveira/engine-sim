#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs("Expected a cost consumer executable and arguments\n", stderr);
        return 2;
    }

    // Fork after the small image is already running. A direct exec from a large
    // driver can charge the driver's resident pages to the trial's ru_maxrss.
    const pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 2;
    }
    if (child == 0) {
        execv(argv[1], argv + 1);
        perror("execv");
        _exit(2);
    }

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("waitpid");
        return 2;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
