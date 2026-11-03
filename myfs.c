#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>
#include <stdint.h>

typedef enum {
    OP_NONE,
    OP_TOUCH,
    OP_CAT,
    OP_COPY,
    OP_MOVE
} Operation;


typedef struct {
    Operation operation;
    int recursive;

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
        "  %s -c [-r] [-i] [-N] [-v] [-p] SOURCE DESTINATION\n"
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
            } else if (strcmp(arg, "-r") == 0) {
                options->recursive = 1;
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

    if (options->recursive && options->operation != OP_COPY) {
        fprintf(stderr, "myfs: -r is valid only with -c\n");
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


static char *join_path(const char *directory, const char *name)
{
    size_t a = strlen(directory);
    size_t b = strlen(name);
    int slash = a > 0 && directory[a - 1] != '/';
    if (a > SIZE_MAX - b - (size_t)slash - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    char *path = malloc(a + b + (size_t)slash + 1);
    if (path == NULL)
        return NULL;
    memcpy(path, directory, a);
    if (slash)
        path[a++] = '/';
    memcpy(path + a, name, b + 1);
    return path;
}

static char *resolve_path(const char *path)
{
    char *resolved = realpath(path, NULL);
    if (resolved != NULL || errno != ENOENT)
        return resolved;
    char *copy = strdup(path);
    if (copy == NULL)
        return NULL;
    char *slash = strrchr(copy, '/');
    const char *name = copy;
    const char *parent = ".";
    if (slash != NULL) {
        *slash = '\0';
        name = slash + 1;
        parent = copy[0] == '\0' ? "/" : copy;
    }
    char *real_parent = realpath(parent, NULL);
    if (real_parent == NULL && errno == ENOENT && strcmp(parent, ".") != 0)
        real_parent = resolve_path(parent);
    if (real_parent == NULL) {
        free(copy);
        return NULL;
    }
    resolved = join_path(real_parent, name);
    free(real_parent);
    free(copy);
    return resolved;
}

static int ask_overwrite(const char *path)
{
    char answer[32];
    printf("Overwrite '%s'? (y/n): ", path);
    fflush(stdout);
    return fgets(answer, sizeof(answer), stdin) != NULL &&
        (answer[0] == 'y' || answer[0] == 'Y');
}

static int copy_file(const char *source, const char *destination,
                     const Options *options)
{
    struct stat source_info, destination_info;
    if (lstat(source, &source_info) == -1) {
        perror(source);
        return 1;
    }
    if (!S_ISREG(source_info.st_mode)) {
        fprintf(stderr, "myfs: source is not a regular file: %s\n", source);
        return 1;
    }
    int exists = lstat(destination, &destination_info) == 0;
    if (!exists && errno != ENOENT) {
        perror(destination);
        return 1;
    }
    if (exists && source_info.st_dev == destination_info.st_dev &&
        source_info.st_ino == destination_info.st_ino) {
        fprintf(stderr, "myfs: source and destination are the same file\n");
        return 1;
    }
    if (exists && options->no_clobber) {
        if (!options->recursive) {
            fprintf(stderr, "myfs: destination already exists: %s\n", destination);
            return 1;
        }
        fprintf(stderr, "myfs: skipping existing destination: %s\n", destination);
        return 0;
    }
    if (exists && options->interactive && !ask_overwrite(destination))
        return 0;
    if (exists && S_ISLNK(destination_info.st_mode)) {
        if (unlink(destination) == -1) {
            perror(destination);
            return 1;
        }
        exists = 0;
    } else if (exists && !S_ISREG(destination_info.st_mode)) {
        fprintf(stderr, "myfs: destination is not a regular file: %s\n", destination);
        return 1;
    }
    int in = open(source, O_RDONLY);
    if (in == -1) {
        perror(source);
        return 1;
    }
    int flags = O_WRONLY | O_CREAT | (exists ? O_TRUNC : O_EXCL);
    int out = open(destination, flags, options->preserve ? source_info.st_mode & 07777 : 0666);
    if (out == -1) {
        perror(destination);
        close(in);
        return 1;
    }
    int failed = 0;
    char buffer[8192];
    for (;;) {
        ssize_t n = read(in, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            perror(source);
            failed = 1;
            break;
        }
        if (n == 0)
            break;
        if (write_all(out, buffer, (size_t)n) == -1) {
            perror(destination);
            failed = 1;
            break;
        }
    }
    if (!failed && options->preserve) {
        struct timespec times[2] = {source_info.st_atim, source_info.st_mtim};
        if (fchmod(out, source_info.st_mode & 07777) == -1 || futimens(out, times) == -1) {
            perror(destination);
            failed = 1;
        }
    }
    if (close(in) == -1) {
        perror(source);
        failed = 1;
    }
    if (close(out) == -1) {
        perror(destination);
        failed = 1;
    }
    if (!failed && options->verbose)
        printf("%s -> %s\n", source, destination);
    return failed;
}

static int copy_node(const char *, const char *, const Options *);

static int copy_directory(const char *source, const char *destination,
                          const Options *options, const struct stat *info)
{
    struct stat dst;
    int exists = lstat(destination, &dst) == 0;
    if (!exists && errno != ENOENT) {
        perror(destination);
        return 1;
    }
    if (exists && !S_ISDIR(dst.st_mode)) {
        fprintf(stderr, "myfs: destination is not a directory: %s\n", destination);
        return 1;
    }
    if (!exists && mkdir(destination, 0777) == -1) {
        perror(destination);
        return 1;
    }
    int failed = 0;
    if (options->verbose && !exists)
        printf("%s -> %s\n", source, destination);
    DIR *dir = opendir(source);
    if (dir == NULL) {
        perror(source);
        return 1;
    }
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                perror(source);
                failed = 1;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char *src = join_path(source, entry->d_name);
        char *dstpath = join_path(destination, entry->d_name);
        if (src == NULL || dstpath == NULL) {
            perror("path allocation");
            free(src);
            free(dstpath);
            failed = 1;
            continue;
        }
        if (copy_node(src, dstpath, options) != 0)
            failed = 1;
        free(src);
        free(dstpath);
    }
    if (closedir(dir) == -1) {
        perror(source);
        failed = 1;
    }
    if (options->preserve) {
        struct timespec times[2] = {info->st_atim, info->st_mtim};
        if (chmod(destination, info->st_mode & 07777) == -1 ||
            utimensat(AT_FDCWD, destination, times, 0) == -1) {
            perror(destination);
            failed = 1;
        }
    }
    return failed;
}

static int copy_node(const char *source, const char *destination,
                     const Options *options)
{
    struct stat info;
    if (lstat(source, &info) == -1) {
        perror(source);
        return 1;
    }
    if (S_ISREG(info.st_mode))
        return copy_file(source, destination, options);
    if (S_ISDIR(info.st_mode))
        return copy_directory(source, destination, options, &info);
    if (S_ISLNK(info.st_mode)) {
        struct stat dst;
        int exists = lstat(destination, &dst) == 0;
        if (!exists && errno != ENOENT) {
            perror(destination);
            return 1;
        }
        if (exists && options->no_clobber) {
            fprintf(stderr, "myfs: skipping existing destination: %s\n", destination);
            return 0;
        }
        if (exists && options->interactive && !ask_overwrite(destination))
            return 0;
        size_t cap = info.st_size > 0 ? (size_t)info.st_size + 1 : 256;
        char *target = malloc(cap);
        if (target == NULL) {
            perror("malloc");
            return 1;
        }
        ssize_t n = readlink(source, target, cap - 1);
        if (n < 0 || (size_t)n == cap - 1) {
            if (n >= 0) errno = ENAMETOOLONG;
            perror(source);
            free(target);
            return 1;
        }
        target[n] = '\0';
        if (exists && unlink(destination) == -1) {
            perror(destination);
            free(target);
            return 1;
        }
        int result = symlink(target, destination);
        if (result == -1)
            perror(destination);
        else if (options->verbose)
            printf("%s -> %s\n", source, destination);
        if (result == 0 && options->preserve) {
            struct timespec times[2] = {info.st_atim, info.st_mtim};
            if (utimensat(AT_FDCWD, destination, times, AT_SYMLINK_NOFOLLOW) == -1)
                perror(destination);
        }
        free(target);
        return result == -1;
    }
    fprintf(stderr, "myfs: unsupported filesystem object: %s\n", source);
    return 1;
}

static int do_copy(const Options *options, char **operands, int operand_count)
{
    (void)operand_count;
    struct stat source_info;
    if (lstat(operands[0], &source_info) == -1) {
        perror(operands[0]);
        return 1;
    }
    if (S_ISDIR(source_info.st_mode) && !options->recursive) {
        fprintf(stderr, "myfs: source is a directory (use -r)\n");
        return 1;
    }
    if (!S_ISDIR(source_info.st_mode))
        return copy_node(operands[0], operands[1], options);
    struct stat dst;
    int exists = lstat(operands[1], &dst) == 0;
    if (!exists && errno != ENOENT) {
        perror(operands[1]);
        return 1;
    }
    if (exists && source_info.st_dev == dst.st_dev && source_info.st_ino == dst.st_ino) {
        fprintf(stderr, "myfs: source and destination are the same directory\n");
        return 1;
    }
    if (exists && !S_ISDIR(dst.st_mode)) {
        fprintf(stderr, "myfs: destination is not a directory: %s\n", operands[1]);
        return 1;
    }
    char *src_real = realpath(operands[0], NULL);
    char *dst_real = resolve_path(operands[1]);
    if (src_real == NULL || dst_real == NULL) {
        perror(src_real == NULL ? operands[0] : operands[1]);
        free(src_real);
        free(dst_real);
        return 1;
    }
    size_t src_len = strlen(src_real);
    if (strncmp(src_real, dst_real, src_len) == 0 &&
        (dst_real[src_len] == '/' || dst_real[src_len] == '\0')) {
        fprintf(stderr, "myfs: cannot copy a directory into itself\n");
        free(src_real);
        free(dst_real);
        return 1;
    }
    free(src_real);
    free(dst_real);
    return copy_directory(operands[0], operands[1], options, &source_info);
}


static int remove_tree(const char *path)
{
    struct stat info;
    if (lstat(path, &info) == -1) {
        perror(path);
        return 1;
    }
    if (!S_ISDIR(info.st_mode)) {
        if (unlink(path) == -1) {
            perror(path);
            return 1;
        }
        return 0;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) {
        perror(path);
        return 1;
    }
    int failed = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                perror(path);
                failed = 1;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char *child = join_path(path, entry->d_name);
        if (child == NULL) {
            perror("path allocation");
            failed = 1;
            continue;
        }
        if (remove_tree(child) != 0)
            failed = 1;
        free(child);
    }
    if (closedir(dir) == -1) {
        perror(path);
        failed = 1;
    }
    if (!failed && rmdir(path) == -1) {
        perror(path);
        failed = 1;
    }
    return failed;
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

    if (lstat(source, &source_info) == -1) {
        perror(source);
        return 1;
    }

    int destination_exists = (lstat(destination, &destination_info) == 0);

    if (!destination_exists && errno != ENOENT) {
        perror(destination);
        return 1;
    }

    if (destination_exists && source_info.st_dev == destination_info.st_dev &&
        source_info.st_ino == destination_info.st_ino) {
        fprintf(stderr, "myfs: source and destination are the same object\n");
        return 1;
    }

    if (S_ISDIR(source_info.st_mode)) {
        char *src_real = realpath(source, NULL);
        char *dst_real = resolve_path(destination);
        if (src_real == NULL || dst_real == NULL) {
            perror(src_real == NULL ? source : destination);
            free(src_real);
            free(dst_real);
            return 1;
        }
        size_t src_len = strlen(src_real);
        if (strncmp(src_real, dst_real, src_len) == 0 &&
            (dst_real[src_len] == '/' || dst_real[src_len] == '\0')) {
            fprintf(stderr, "myfs: cannot move a directory into itself\n");
            free(src_real);
            free(dst_real);
            return 1;
        }
        free(src_real);
        free(dst_real);
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
    copy_options.recursive = S_ISDIR(source_info.st_mode);

    char *copy_operands[] = {
        (char *)source,
        (char *)destination
    };

    if (do_copy(&copy_options, copy_operands, 2) != 0) {
        return 1;
    }

    int remove_failed = S_ISDIR(source_info.st_mode)
        ? remove_tree(source)
        : (unlink(source) == -1 ? (perror(source), 1) : 0);
    if (remove_failed) {
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
