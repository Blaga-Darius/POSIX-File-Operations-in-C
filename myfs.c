#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

typedef enum {
    OP_NONE,
    OP_TOUCH,
    OP_CAT,
    OP_COPY,
    OP_MOVE
} Operation;


typedef struct {
    Operation operation;

    int no_create;

    int number_all;
    int number_nonempty;
    int show_ends;
    int show_tabs;
    int squeeze_blank;

    int interactive;
    int no_clobber;
    int verbose;
    int preserve;
} Options;


static void usage(const char *program)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s -t [-C] FILE...\n"
        "  %s -a [-n] [-b] [-E] [-S] [-s] [FILE...]\n"
        "  %s -c [-i] [-N] [-v] [-p] SOURCE DESTINATION\n"
        "  %s -m [-i] [-N] [-v] SOURCE DESTINATION\n",
        program, program, program, program);
}


static int parse_options(
    int argc,
    char *argv[],
    Options *options,
    char ***operands_out,
    int *operand_count_out)
{
    char **operands = calloc((size_t)argc, sizeof(char *));

    if (operands == NULL) {
        perror("calloc");
        return -1;
    }

    int operand_count = 0;
    int end_options = 0;

    memset(options, 0, sizeof(*options));

    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];

        if (!end_options && strcmp(arg, "--") == 0) {
            end_options = 1;
            continue;
        }

        if (!end_options && arg[0] == '-' && arg[1] != '\0') {

            if (strcmp(arg, "-t") == 0 ||
                strcmp(arg, "-a") == 0 ||
                strcmp(arg, "-c") == 0 ||
                strcmp(arg, "-m") == 0) {

                if (options->operation != OP_NONE) {
                    fprintf(stderr,
                        "myfs: select exactly one operation\n");
                    goto error;
                }

                if (strcmp(arg, "-t") == 0) {
                    options->operation = OP_TOUCH;
                } else if (strcmp(arg, "-a") == 0) {
                    options->operation = OP_CAT;
                } else if (strcmp(arg, "-c") == 0) {
                    options->operation = OP_COPY;
                } else {
                    options->operation = OP_MOVE;
                }
            }

            else if (strcmp(arg, "-C") == 0) {
                options->no_create = 1;
            }

            else if (strcmp(arg, "-n") == 0) {
                options->number_all = 1;
            } else if (strcmp(arg, "-b") == 0) {
                options->number_nonempty = 1;
            } else if (strcmp(arg, "-E") == 0) {
                options->show_ends = 1;
            } else if (strcmp(arg, "-S") == 0) {
                options->show_tabs = 1;
            } else if (strcmp(arg, "-s") == 0) {
                options->squeeze_blank = 1;
            }

            else if (strcmp(arg, "-i") == 0) {
                options->interactive = 1;
            } else if (strcmp(arg, "-N") == 0) {
                options->no_clobber = 1;
            } else if (strcmp(arg, "-v") == 0) {
                options->verbose = 1;
            } else if (strcmp(arg, "-p") == 0) {
                options->preserve = 1;
            }

            else {
                fprintf(stderr,
                    "myfs: unknown option: %s\n", arg);
                goto error;
            }
        } else {
            operands[operand_count++] = arg;
        }
    }

    if (options->operation == OP_NONE) {
        fprintf(stderr, "myfs: an operation is required\n");
        goto error;
    }

    if (options->no_create &&
        options->operation != OP_TOUCH) {
        fprintf(stderr, "myfs: -C is valid only with -t\n");
        goto error;
    }

    int has_cat_options =
        options->number_all ||
        options->number_nonempty ||
        options->show_ends ||
        options->show_tabs ||
        options->squeeze_blank;

    if (has_cat_options && options->operation != OP_CAT) {
        fprintf(stderr, "myfs: cat options require -a\n");
        goto error;
    }

    if ((options->interactive ||
         options->no_clobber ||
         options->verbose) &&
        options->operation != OP_COPY &&
        options->operation != OP_MOVE) {
        fprintf(stderr, "myfs: -i, -N and -v require -c or -m\n");
        goto error;
    }

    if (options->preserve &&
        options->operation != OP_COPY) {
        fprintf(stderr, "myfs: -p is valid only with -c\n");
        goto error;
    }

    if (options->interactive && options->no_clobber) {
        fprintf(stderr,
            "myfs: -i and -N cannot be used together\n");
        goto error;
    }

    *operands_out = operands;
    *operand_count_out = operand_count;

    return 0;

error:
    free(operands);
    return -1;
}


static int do_touch(
    const Options *options,
    char **operands,
    int operand_count)
{
    int failures = 0;

    for (int i = 0; i < operand_count; i++) {
        const char *path = operands[i];

        if (utimensat(AT_FDCWD, path, NULL, 0) == 0) {
            continue;
        }

        if (errno != ENOENT || options->no_create) {
            perror(path);
            failures = 1;
            continue;
        }


        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);

        if (fd == -1) {

            if (errno == EEXIST &&
                utimensat(AT_FDCWD, path, NULL, 0) == 0) {
                continue;
            }

            perror(path);
            failures = 1;
            continue;
        }

        if (close(fd) == -1) {
            perror(path);
            failures = 1;
        }
    }

    return failures;
}


static int write_all(int fd, const void *buffer, size_t count)
{
    const char *data = buffer;
    size_t written = 0;

    while (written < count) {
        ssize_t result = write(fd, data + written, count - written);

        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (result == 0) {
            errno = EIO;
            return -1;
        }

        written += (size_t)result;
    }

    return 0;
}


static int print_line_number(long number)
{
    char buffer[64];

    int length = snprintf(
        buffer,
        sizeof(buffer),
        "%6ld\t",
        number
    );

    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        errno = EOVERFLOW;
        return -1;
    }

    return write_all(STDOUT_FILENO, buffer, (size_t)length);
}


static int do_cat(
    const Options *options,
    char **operands,
    int operand_count)
{
    char buffer[4096];

    long line_number = 0;
    int previous_line_blank = 0;
    int failures = 0;

    int file_count = operand_count > 0 ? operand_count : 1;
    int at_line_start = 1;
    int line_has_content = 0;

    for (int file_index = 0; file_index < file_count; file_index++) {
        int fd;

        if (operand_count == 0) {
            fd = STDIN_FILENO;
        } else {
            fd = open(operands[file_index], O_RDONLY);

            if (fd == -1) {
                perror(operands[file_index]);
                failures = 1;
                continue;
            }
        }

        int skip_blank_line = 0;

        while (1) {
            ssize_t bytes_read = read(fd, buffer, sizeof(buffer));

            if (bytes_read == -1) {
                if (errno == EINTR) {
                    continue;
                }

                perror("read");
                failures = 1;
                break;
            }

            if (bytes_read == 0) {
                break;
            }

            for (ssize_t i = 0; i < bytes_read; i++) {
                char ch = buffer[i];

                if (at_line_start) {
                    skip_blank_line =
                        options->squeeze_blank &&
                        previous_line_blank &&
                        ch == '\n';

                    if (skip_blank_line) {
                        continue;
                    }

                    if (options->number_all ||
                        (options->number_nonempty && ch != '\n')) {

                        if (print_line_number(++line_number) == -1) {
                            perror("write");
                            failures = 1;
                            break;
                        }
                    }

                    at_line_start = 0;
                    line_has_content = 0;
                }

                if (ch == '\t' && options->show_tabs) {
                    if (write_all(STDOUT_FILENO, "^I", 2) == -1) {
                        perror("write");
                        failures = 1;
                        break;
                    }
                } else {
                    if (ch == '\n' && options->show_ends) {
                        if (write_all(STDOUT_FILENO, "$", 1) == -1) {
                            perror("write");
                            failures = 1;
                            break;
                        }
                    }

                    if (write_all(STDOUT_FILENO, &ch, 1) == -1) {
                        perror("write");
                        failures = 1;
                        break;
                    }
                }

                if (ch != '\n') {
                    line_has_content = 1;
                } else {
                    previous_line_blank = !line_has_content;
                    at_line_start = 1;
                }
            }

            if (failures) {
                break;
            }
        }

        if (operand_count > 0 && close(fd) == -1) {
            perror("close");
            failures = 1;
        }

        if (failures) {
            break;
        }
    }

    return failures;
}


static int do_copy(
    const Options *options,
    char **operands,
    int operand_count)
{
    (void)operand_count;

    const char *source = operands[0];
    const char *destination = operands[1];

    struct stat source_info;
    struct stat destination_info;

    if (stat(source, &source_info) == -1) {
        perror(source);
        return 1;
    }

    if (!S_ISREG(source_info.st_mode)) {
        fprintf(stderr, "myfs: source is not a regular file: %s\n",
                source);
        return 1;
    }

    if (stat(destination, &destination_info) == 0) {
        if (source_info.st_dev == destination_info.st_dev &&
            source_info.st_ino == destination_info.st_ino) {
            fprintf(stderr,
                    "myfs: source and destination are the same file\n");
            return 1;
        }

        if (options->no_clobber) {
            fprintf(stderr,
                    "myfs: destination already exists: %s\n",
                    destination);
            return 1;
        }

        if (options->interactive) {
            char answer[32];

            printf("Overwrite '%s'? (y/n): ", destination);
            fflush(stdout);

            if (fgets(answer, sizeof(answer), stdin) == NULL ||
                (answer[0] != 'y' && answer[0] != 'Y')) {
                printf("myfs: copy cancelled\n");
                return 0;
            }
        }
    } else if (errno != ENOENT) {
        perror(destination);
        return 1;
    }

    int source_fd = open(source, O_RDONLY);

    if (source_fd == -1) {
        perror(source);
        return 1;
    }

    int destination_flags = O_WRONLY | O_CREAT;

    if (options->no_clobber) {
        destination_flags |= O_EXCL;
    } else {
        destination_flags |= O_TRUNC;
    }

    mode_t destination_mode = options->preserve
        ? source_info.st_mode & 07777
        : 0666;

    int destination_fd = open(
        destination,
        destination_flags,
        destination_mode
    );

    if (destination_fd == -1) {
        perror(destination);
        close(source_fd);
        return 1;
    }

    int failed = 0;
    char buffer[4096];

    while (1) {
        ssize_t bytes_read = read(source_fd, buffer, sizeof(buffer));

        if (bytes_read == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("read");
            failed = 1;
            break;
        }

        if (bytes_read == 0) {
            break;
        }

        if (write_all(destination_fd, buffer, (size_t)bytes_read) == -1) {
            perror("write");
            failed = 1;
            break;
        }
    }

    if (!failed && options->preserve) {
        if (fchmod(destination_fd, source_info.st_mode & 07777) == -1) {
            perror("fchmod");
            failed = 1;
        }

        struct timespec times[2];

        times[0] = source_info.st_atim;
        times[1] = source_info.st_mtim;

        if (!failed && futimens(destination_fd, times) == -1) {
            perror("futimens");
            failed = 1;
        }
    }

    if (close(source_fd) == -1) {
        perror(source);
        failed = 1;
    }

    if (close(destination_fd) == -1) {
        perror(destination);
        failed = 1;
    }

    if (failed) {
        return 1;
    }

    if (options->verbose) {
        printf("%s -> %s\n", source, destination);
    }

    return 0;
}


static int do_move(
    const Options *options,
    char **operands,
    int operand_count)
{
    (void)operand_count;

    const char *source = operands[0];
    const char *destination = operands[1];

    struct stat source_info;
    struct stat destination_info;

    if (stat(source, &source_info) == -1) {
        perror(source);
        return 1;
    }

    int destination_exists = (lstat(destination, &destination_info) == 0);

    if (!destination_exists && errno != ENOENT) {
        perror(destination);
        return 1;
    }

    if (destination_exists) {
        if (options->no_clobber) {
            fprintf(stderr,
                    "myfs: destination already exists: %s\n",
                    destination);
            return 1;
        }

        if (options->interactive) {
            char answer[32];

            printf("Overwrite '%s'? (y/n): ", destination);
            fflush(stdout);

            if (fgets(answer, sizeof(answer), stdin) == NULL ||
                (answer[0] != 'y' && answer[0] != 'Y')) {
                printf("myfs: move cancelled\n");
                return 0;
            }
        }
    }

    if (rename(source, destination) == 0) {
        if (options->verbose) {
            printf("%s -> %s\n", source, destination);
        }

        return 0;
    }

    if (errno != EXDEV) {
        perror("rename");
        return 1;
    }

    Options copy_options = *options;
    copy_options.operation = OP_COPY;
    copy_options.verbose = 0;
    copy_options.interactive = 0;
    copy_options.no_clobber = 0;
    copy_options.preserve = 1;

    char *copy_operands[] = {
        (char *)source,
        (char *)destination
    };

    if (do_copy(&copy_options, copy_operands, 2) != 0) {
        return 1;
    }

    if (unlink(source) == -1) {
        perror(source);
        return 1;
    }

    if (options->verbose) {
        printf("%s -> %s\n", source, destination);
    }

    return 0;
}


int main(int argc, char *argv[])
{
    Options options;
    char **operands = NULL;
    int operand_count = 0;

    if (parse_options(
            argc,
            argv,
            &options,
            &operands,
            &operand_count) == -1) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    int result;

    switch (options.operation) {
        case OP_TOUCH:
            if (operand_count < 1) {
                fprintf(stderr,
                    "myfs: -t requires at least one file\n");
                usage(argv[0]);
                free(operands);
                return EXIT_FAILURE;
            }

            result = do_touch(&options, operands, operand_count);
            break;

        case OP_CAT:
            result = do_cat(&options, operands, operand_count);
            break;

        case OP_COPY:
        case OP_MOVE:
            if (operand_count != 2) {
                fprintf(stderr,
                    "myfs: expected a source and a destination\n");
                usage(argv[0]);
                free(operands);
                return EXIT_FAILURE;
            }

            if (options.operation == OP_COPY) {
                result = do_copy(
                    &options, operands, operand_count);
            } else {
                result = do_move(
                    &options, operands, operand_count);
            }
            break;

        default:
            result = EXIT_FAILURE;
            break;
    }

    free(operands);

    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}