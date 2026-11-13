#define _POSIX_C_SOURCE 200809L

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <errno.h>
#include <stddef.h>

#define BUFFER_SIZE 4096

extern char **environ;

typedef struct {
    int done;
    int status;
    size_t length;
    char line[BUFFER_SIZE];
    char result[32];
} SharedData;

static void error(const char *message)
{
    size_t length = 0;
    while (message[length] != '\0') length++;
    write(STDERR_FILENO, message, length);
}

static void make_name(char *name, const char *prefix)
{
    int i = 0;
    while (prefix[i] != '\0') {
        name[i] = prefix[i];
        i++;
    }

    int pid = (int)getpid();
    char digits[20];
    int count = 0;
    do {
        digits[count++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0);

    while (count > 0) name[i++] = digits[--count];
    name[i] = '\0';
}

static int read_line(char *line, size_t size)
{
    size_t length = 0;
    char c;

    while (1) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            if (length == 0) return 0;
            break;
        }
        if (c == '\n') break;
        if (length + 1 >= size) return -1;
        line[length++] = c;
    }

    line[length] = '\0';
    return 1;
}

static int wait_sem(sem_t *sem)
{
    while (sem_wait(sem) < 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

static int send_chunk(SharedData *data, sem_t *request, sem_t *reply,
                      const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) data->line[i] = text[i];
    data->length = length;
    data->status = 0;

    if (sem_post(request) < 0 || wait_sem(reply) < 0) return -1;
    if (data->status < 0) return -1;

    if (data->status == 1) {
        size_t size = 0;
        while (data->result[size] != '\0') size++;
        if (write(STDOUT_FILENO, data->result, size) != (ssize_t)size)
            return -1;
    }
    return 0;
}

static void cleanup(SharedData *data, sem_t *request, sem_t *reply,
                    const char *shm_name, const char *request_name,
                    const char *reply_name)
{
    if (request != SEM_FAILED) sem_close(request);
    if (reply != SEM_FAILED) sem_close(reply);
    if (data != MAP_FAILED) munmap(data, sizeof(SharedData));
    sem_unlink(request_name);
    sem_unlink(reply_name);
    shm_unlink(shm_name);
}

int main(void)
{
    char filename[BUFFER_SIZE];
    if (read_line(filename, sizeof(filename)) != 1 || filename[0] == '\0') {
        error("Invalid file name\n");
        return 1;
    }

    char shm_name[64], request_name[64], reply_name[64];
    make_name(shm_name, "/lab3_v3_shm_");
    make_name(request_name, "/lab3_v3_req_");
    make_name(reply_name, "/lab3_v3_rep_");

    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        error("shm_open failed\n");
        return 1;
    }
    if (ftruncate(fd, sizeof(SharedData)) < 0) {
        error("ftruncate failed\n");
        close(fd);
        shm_unlink(shm_name);
        return 1;
    }

    SharedData *data = mmap(NULL, sizeof(SharedData),
                            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        error("mmap failed\n");
        shm_unlink(shm_name);
        return 1;
    }
    data->done = 0;
    data->status = 0;

    sem_t *request = sem_open(request_name, O_CREAT | O_EXCL, 0600, 0);
    sem_t *reply = sem_open(reply_name, O_CREAT | O_EXCL, 0600, 0);
    if (request == SEM_FAILED || reply == SEM_FAILED) {
        error("sem_open failed\n");
        cleanup(data, request, reply, shm_name, request_name, reply_name);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        error("fork failed\n");
        cleanup(data, request, reply, shm_name, request_name, reply_name);
        return 1;
    }
    if (pid == 0) {
        char *const args[] = {"./child", filename, shm_name,
                              request_name, reply_name, NULL};
        execve("./child", args, environ);
        error("execve failed\n");
        data->status = -1;
        sem_post(reply);
        _exit(1);
    }

    int failed = 0;
    if (wait_sem(reply) < 0 || data->status != 0) failed = 1;

    char buffer[BUFFER_SIZE];
    size_t length = 0;
    int unfinished = 0;

    while (!failed) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            failed = 1;
            break;
        }
        if (n == 0) break;

        buffer[length++] = c;
        if (c != '\n') unfinished = 1;

        if (length == BUFFER_SIZE || c == '\n') {
            if (send_chunk(data, request, reply, buffer, length) < 0) {
                failed = 1;
                break;
            }
            length = 0;
        }
        if (c == '\n') unfinished = 0;
    }

    if (!failed && unfinished) {
        buffer[length++] = '\n';
        if (send_chunk(data, request, reply, buffer, length) < 0) failed = 1;
    }

    data->done = 1;
    if (sem_post(request) < 0) failed = 1;

    int child_status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &child_status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0 || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) failed = 1;

    cleanup(data, request, reply, shm_name, request_name, reply_name);
    return failed ? 1 : 0;
}