#include <stddef.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#define true 1
#define false 0

enum PipechainConnector {
    CONN_ANY, CONN_FAILURE, CONN_SUCCESS
};

struct Redirect {
    char *path;
    int fd;
    int flags;
};

struct SimpleCommand {
    char **argv;
    struct Redirect *redirect;
    size_t nredir;
    struct SimpleCommand *next;
};

struct Pipechain {
    struct Pipechain *next;
    struct SimpleCommand *simple_commands;
    int reverse_status;
    enum PipechainConnector connector;
};

int builtin_cd(char **);
int builtin_exit(char **);

char *builtin_str[] = {
  "cd",
  "exit"
};

int (*builtin_func[]) (char **) = {
  &builtin_cd,
  &builtin_exit
};
int builtin_count = sizeof(builtin_func) / sizeof(builtin_func[0]);

int builtin_cd(char **args) {
    if (args[0] == NULL) {
        char *msg = "builtin: expect argument to \"cd\"";
        write(STDERR_FILENO, msg, strlen(msg));
        return 1;
    }
    if (chdir(args[0]) != 0) {
        perror("builtin");
        return 1;
    }
    return 0;
}

int builtin_exit(char **args) {
    return -1;
}

int movfd(int old, int new) {
    if (old == new) {
        return new;
    }
    if (dup2(old, new) == -1) {
        return -1;
    }
    if (close(old) == -1) {
        return -1;
    }
    return new;
}

void exec_simple_command(struct SimpleCommand *cmd) {
    
}

int execute_pipechain(struct Pipechain *chain) {
    struct SimpleCommand *current = chain->simple_commands;

    if (current != NULL && current->next == NULL) {
        // directly calling builtin functions when there are no pipes.
        // this is important for cd and exit.
        for (int i = 0; i < builtin_count; i++) {
            if (strcmp(current->argv[0], builtin_str[i]) == 0) {
                return builtin_func[i](&(current->argv[1]));
            }
        }
    }

    size_t childern_capacity = 8;
    size_t childern_nmemb = 0;
    pid_t *childern = calloc(8, sizeof(pid_t));
    if (childern == NULL) {
        perror("malloc");
        _exit(EXIT_FAILURE);
    }

    int inputfd = STDIN_FILENO;
    while (current != NULL) {
        int pipefd[2];
        int has_next = current->next != NULL;
        if (has_next) {
            pipe(pipefd);
        }
        
        pid_t cpid = fork();
        if (cpid == 0) {  /* Child. Forked out to execute command */
            movfd(inputfd, STDIN_FILENO);
            if (has_next) {
                if (close(pipefd[0]) == -1  /* Close read end of pipe */
                    || movfd(pipefd[1], STDOUT_FILENO) == -1) /* Move write end of pipe */ {
                    perror("close");
                    _exit(EXIT_FAILURE);
                }
            }

            // builtins
            for (int i = 0; i < builtin_count; i++) {
                if (strcmp(current->argv[0], builtin_str[i]) == 0) {
                    int status = builtin_func[i](&(current->argv[1]));
                    if (status < 0) {
                        _exit(0);
                    }
                    _exit(status);
                }
            }

            execvp(current->argv[0], current->argv);
        } else {  /* Parent. Keep processing in the pipechain */
            if (inputfd != STDIN_FILENO) {
                if (close(inputfd) == -1) {
                    perror("close");
                    break;
                }
            }
            if (has_next) {
                if (close(pipefd[1]) == -1) { /* Close write end of pipe */
                    perror("close");
                    break;
                }
                inputfd = pipefd[0];
            }
            if (childern_nmemb > childern_capacity) {
                pid_t *new_childern = reallocarray(childern, childern_capacity * 2, sizeof(pid_t));
                if (new_childern == NULL) {
                    perror("reallocarray");
                    _exit(EXIT_FAILURE);
                }
                childern_capacity *= 2;
                childern = new_childern;
            }
            childern[childern_nmemb] = cpid;
            childern_nmemb++;
            
            current = current->next;
        }
    }
    int status;
    for (size_t i = 0; i < childern_nmemb; i++) {
        waitpid(childern[i], &status, 0);
        status = WEXITSTATUS(status);
    }
    free(childern);

    if (chain->reverse_status) {
        return !status;
    }
    return status;
}

int execute_command(struct Pipechain *cmd) {
    int status = 0;
    while (cmd != NULL) {
        status = execute_command(cmd);
        
        if (status == 0 && cmd->connector == CONN_FAILURE
            || status != 0 && cmd->connector == CONN_SUCCESS) {
            return status;
        }
        cmd = cmd->next;
    }
    return status;
}

/*
int extend_compound_command(struct CompoundCommand *cmd, char *line)
{
    int inside_squote = 0;
    int inside_dquote = 0;
    int inside_file = 0;

    if (cmd->pipechain_count < 1) {
        struct Pipechain *newpipechain = malloc(sizeof(struct Pipechain));
        if (newpipechain == NULL) {
            perror("malloc");
            exit(EXIT_FAILURE);
        }
        newpipechain->first = NULL;
        newpipechain->reverse_status = false;
        newpipechain->final = false;
        cmd->pipechain = newpipechain;
        cmd->pipechain_count = 1;
    }
}
*/

/**
 * @brief A reimplementation of the getline function using low-level IO.
 */
ssize_t get_line(char **buf, size_t *bufsize, int fd)
{
    char chr;
    ssize_t readret, count = 0;
    if (*buf == NULL) {
        *buf = malloc(64);
        if (*buf == NULL) {
            return -1;
        }
    }
    while (true) {
        readret = read(fd, &chr, 1);
        if (readret == 0) {
            break;
        }
        if (readret == -1) {
            return -1;
        }

        (*buf)[count] = chr;
        count++;
        if (count == *bufsize) {
            char *new_buf = reallocarray(buf, *bufsize * 2, sizeof(char));
            if (new_buf == NULL) {
                return -1;
            }
            *bufsize = *bufsize * 2;
            *buf = new_buf;
        }

        if (chr == '\n') {
            break;
        }
    }
    (*buf)[count] = '\0';
    return count;
}

int main(int argc, char *argv[], char *envp[])
{

}
