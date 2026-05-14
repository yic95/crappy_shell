#include <asm-generic/errno-base.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>

#define true 1
#define false 0

enum PipechainConnector {
    CONN_FAILURE, CONN_SUCCESS
};

struct SimpleCommand {
    int oredir_append;
    char **argv;
    char *iredir_path;
    char *oredir_path;
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

int builtin_cd(char **args)
{
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

int builtin_exit(char **args)
{
    return -1;
}

int movfd(int old, int new)
{
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

void setup_redirect(char *ipath, char *opath, int *ifd, int *ofd)
{
    if (ifd != NULL) {
        *ifd = -1;
        if (ipath != NULL) {
            *ifd = open(ipath, O_RDONLY);
            if (*ifd >= 0)
                dup2(*ifd, STDIN_FILENO);
        }
    }
    if (ofd != NULL) {
        int append = *ofd;
        *ofd = -1;
        if (opath != NULL) {
            if (append) {
                *ofd = open(opath, O_WRONLY|O_CREAT|O_APPEND);
            } else {
                *ofd = open(opath, O_WRONLY|O_CREAT|O_TRUNC);
            }
            if (*ofd >= 0)
                dup2(*ofd, STDOUT_FILENO);
        }
    }
}

int execute_pipechain(struct Pipechain *chain) {
    struct SimpleCommand *current = chain->simple_commands;

    if (current != NULL && current->next == NULL) {
        // directly calling builtin functions when there are no pipes.
        // this is important for cd and exit.
        for (int i = 0; i < builtin_count; i++) {
            if (strcmp(current->argv[0], builtin_str[i]) == 0) {
                int original_stdin = dup(STDIN_FILENO);
                int original_stdout = dup(STDOUT_FILENO);
                int ifd, ofd;
                ofd = current->oredir_append;
                setup_redirect(current->iredir_path, current->oredir_path, &ifd, &ofd);
                int status = builtin_func[i](&(current->argv[1]));
                dup2(original_stdin, STDIN_FILENO);
                dup2(original_stdout, STDOUT_FILENO);
                if (ifd >= 0) 
                    close(ifd);
                if (ofd >= 0)
                    close(ofd);
                close(original_stdin);
                close(original_stdout);
                return status;
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
                if (close(pipefd[0]) == -1) {  /* Close read end of pipe */
                    perror("close");
                    exit(EXIT_FAILURE);
                }
                movfd(pipefd[1], STDOUT_FILENO);
            }

            int ofd = current->oredir_append;
            setup_redirect(current->iredir_path, current->oredir_path, NULL, &ofd);
            // builtins
            for (int i = 0; i < builtin_count; i++) {
                if (strcmp(current->argv[0], builtin_str[i]) == 0) {
                    int status = builtin_func[i](&(current->argv[1]));
                    if (status < 0) {
                        exit(0);
                    }
                    exit(status);
                }
            }

            execvp(current->argv[0], current->argv);
            perror("execvp");
            if (errno == ENOENT) {
                exit(127);
            }
            if (errno == EACCES) {
                exit(126);
            }
            exit(1);
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

int execute_command(struct Pipechain *cmd)
{
    int status = 0;
    while (cmd != NULL) {
        status = execute_pipechain(cmd);
        
        if (status == 0 && cmd->connector == CONN_FAILURE
            || status != 0 && cmd->connector == CONN_SUCCESS) {
            return status;
        }
        cmd = cmd->next;
    }
    return status;
}

// Helper to skip whitespace
const char* skip_ws(const char *s) {
    while (*s && isspace(*s)) 
        s++;
    return s;
}

// Extract next token without modifying original string
char* get_token(const char **s) {
    *s = skip_ws(*s);
    if (**s == '\0')
        return NULL;

    const char* start = *s;
    int only_standard_words = false;
    if (**s == '\'') {
        (*s)++;
        while (**s && **s != '\'') {
            (*s)++;
        }
        if (**s) {
            (*s)++;
        }
        only_standard_words = true;
        // POSIX says that we shouldn't delimit tokens at end of quote.
    }
    // Handle metacharacters as single tokens
    if (!only_standard_words
        && (strncmp(*s, "&&", 2) == 0
            || strncmp(*s, "||", 2) == 0
            || strncmp(*s, ">>", 2) == 0)) {
        *s += 2;
    } else if (!only_standard_words && strchr("|<>", **s)) {
        (*s)++;
    } else {
        // Standard word
        // TODO handle double quotes
        while (**s && !isspace(**s) && !strchr("|<>!&", **s)) {
            // backslash escape
            if (**s == '\\' && *((*s)+1)) {
                (*s)++;
            }
            (*s)++;
        }
    }
    size_t len = *s - start;
    if (len == 0)
        return NULL;
    char *token = malloc(len + 1);
    memcpy(token, start, len);
    token[len] = '\0';
    return token;
}

void remove_squote(char *s)
{
    int outside_quote = true;
    while (*s) {
        if (outside_quote && *s == '\\' && *(s + 1)) {
            s += 2;
            continue;
        }
        if (*s == '\'') {
            outside_quote = !outside_quote;
            char *c = s;
            while (*c) {
                *c = *(c + 1);
                c++;
            }
        }
        s++;
    }
}

struct Pipechain *parse_line(const char *line)
{
    const char *cursor = line;
    struct Pipechain *head = NULL;
    struct Pipechain *previous_pipe = NULL;

    while (true) {
        cursor = skip_ws(cursor);
        if (*cursor == '\0') {
            break;
        }
        struct Pipechain *current_pipe = calloc(1,sizeof(struct Pipechain));
        if (head == NULL)
            head = current_pipe;
        if (previous_pipe != NULL) {
            previous_pipe->next = current_pipe;
        }
        previous_pipe = current_pipe;
    
        current_pipe->reverse_status = false;
        if (*cursor == '!') {
            current_pipe->reverse_status = true;
            cursor++;
        }

        struct SimpleCommand *previous_cmd = NULL;
        int pipeline_done = false;
        while (!pipeline_done) {
            struct SimpleCommand *current_cmd = calloc(1, sizeof(struct SimpleCommand));
            if (current_pipe->simple_commands == NULL)
                current_pipe->simple_commands = current_cmd;
            if (previous_cmd != NULL)
                previous_cmd->next = current_cmd;
            previous_cmd = current_cmd;

            char *token;
            size_t argv_capacity = 8;
            int argc = 0;
            current_cmd->argv = calloc(argv_capacity, sizeof(char *));
            while (1) {
                token = get_token(&cursor);
                if (token == NULL) {
                    pipeline_done = 1;
                    break;
                }
                if (strcmp("|", token) == 0) {
                    free(token);
                    break;
                }
                if (strcmp("&&", token) == 0) {
                    free(token);
                    current_pipe->connector = CONN_SUCCESS;
                    pipeline_done = 1;
                    break;
                }
                if (strcmp("||", token) == 0) {
                    free(token);
                    current_pipe->connector = CONN_FAILURE;
                    pipeline_done = 1;
                    break;
                }
                if (strcmp(">>", token) == 0
                    || strchr("<>", *token)) {
                    char *fname = get_token(&cursor);
                    remove_squote(fname);
                    if (strlen(fname) == 0) {
                        free(token);
                        free(fname);
                        pipeline_done = 1;
                        break;
                    }
                    if (strchr(token, '>') != NULL) {
                        free(current_cmd->oredir_path);
                        current_cmd->oredir_path = fname;
                        if (strcmp(">>", token) == 0) {
                            current_cmd->oredir_append = true;
                        } else {
                            current_cmd->oredir_append = false;
                        }
                    }
                    continue;
                }
                if (argc > argv_capacity - 1) {
                    argv_capacity *= 2;
                    current_cmd->argv = reallocarray(current_cmd->argv, argv_capacity, sizeof(char *));
                    if (current_cmd->argv == NULL) {
                        perror("calloc");
                        exit(1);
                    }
                }
                remove_squote(token);
                current_cmd->argv[argc] = token;
                argc++;
            }

            current_cmd->argv[argc] = NULL;
        }
    }

    return head;
}

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

void print_cmd(struct Pipechain *walk)
{
    while (walk) {
        struct SimpleCommand *walksc = walk->simple_commands;
        while (walksc) {
            for (int i = 0; walksc->argv[i]; i++)
                printf("%s ", walksc->argv[i]);
            if (walksc->iredir_path) {
                printf("<%s ", walksc->iredir_path);
            }
            if (walksc->oredir_path) {
                printf(">%s ", walksc->oredir_path);
            }
            if (walksc->next)
                printf("| ");
            walksc = walksc->next;
        }
        if (walk->next) {
            printf("%s", walk->connector == CONN_SUCCESS ? "&&" : "||");
        }
        walk = walk->next;
    }
}

void free_cmd(struct Pipechain *cmd)
{
    while (cmd) {
        struct Pipechain *next_pl = cmd->next;
        struct SimpleCommand *sc = cmd->simple_commands;
        while (sc) {
            struct SimpleCommand *next = sc->next;

            for (int i = 0; sc->argv[i] == NULL; i++) {
                free(sc->argv[i]);
            }
            free(sc->oredir_path);
            free(sc->iredir_path);
            
            free(sc);
            sc = next;
        }
        free(cmd);
        cmd = next_pl;
    }
}

int main(int argc, char *argv[], char *envp[])
{
    struct Pipechain *cmd = parse_line("echo 'Hello, world!'|cat&&echo 'hi' | cat");
    execute_command(cmd);
    free_cmd(cmd);
}
