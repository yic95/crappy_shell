#include <err.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(int argc, char **argv, char **envp)
{
    int pipefd[2];
    char buf;
    pid_t cpid;

    if (argc != 2) {
        exit(EXIT_FAILURE);
    }

    if (pipe(pipefd) == -1) {
        err(EXIT_FAILURE, "pipe");
    }

    cpid = fork();
    if (cpid == -1) {
        err(EXIT_FAILURE, "fork");
    }
    if (cpid == 0) {
        // child
        if (close(pipefd[1]) == -1) {
            err(EXIT_FAILURE, "close");
        }
        while (read(pipefd[0], &buf, 1) > 0) {
            if (write(STDOUT_FILENO, &buf, 1) != 1) {
                err(EXIT_FAILURE, "write");
            }
        }
        if (write(STDOUT_FILENO, "\n", 1) != 1) {
            err(EXIT_FAILURE, "write");
        }
        if (close(pipefd[0]) == -1) {
            err(EXIT_FAILURE, "close");
        }
        _exit(EXIT_SUCCESS);
    } else {
        if (close(pipefd[0]) == -1) {
            err(EXIT_FAILURE, "close");
        }
        if (write(pipefd[1], argv[1], strlen(argv[1])) != strlen(argv[1])) {
            err(EXIT_FAILURE, "write");
        }
        if (close(pipefd[1]) == -1) {
            err(EXIT_FAILURE, "close");
        }
        if (wait(NULL) == -1) {
            err(EXIT_FAILURE, "wait");
        }
        exit(EXIT_SUCCESS);
    }
}
