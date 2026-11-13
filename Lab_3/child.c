#define _POSIX_C_SOURCE 200809L

#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <limits.h>
#include <errno.h>
#include <stddef.h>

#define BUFFER_SIZE 4096

typedef struct {
    int done;
    int status;
    size_t length;
    char line[BUFFER_SIZE];
    char result[32];
} SharedData;

typedef struct {
    long long number;
    int sign;
    int token_state;
    int has_value;
    int quotient;
} DivisionState;

static int wait_sem(sem_t *sem)
{
    while (sem_wait(sem) < 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

static int write_all(int fd, const char *text, size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t n = write(fd, text + written, length - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        written += (size_t)n;
    }
    return 0;
}

static size_t format_number(int value, char *text)
{
    long long number = value;
    size_t length = 0;
    char digits[32];
    int count = 0;

    if (number < 0) {
        text[length++] = '-';
        number = -number;
    }
    do {
        digits[count++] = (char)('0' + number % 10);
        number /= 10;
    } while (number != 0);

    while (count > 0) text[length++] = digits[--count];
    text[length++] = '\n';
    text[length] = '\0';
    return length;
}

static int finish_number(DivisionState *state)
{
    if (state->token_state == 0) return 0;
    if (state->token_state != 2) return -1;

    int value = (int)(state->sign * state->number);
    if (!state->has_value) {
        state->quotient = value;
        state->has_value = 1;
    } else {
        if (value == 0) return -2;
        if (state->quotient == INT_MIN && value == -1) return -1;
        state->quotient /= value;
    }

    state->token_state = 0;
    state->number = 0;
    state->sign = 1;
    return 0;
}

static int process_character(DivisionState *state, char c,
                             SharedData *data, int file)
{
    if (c == '\n') {
        int result = finish_number(state);
        if (result < 0) return result;
        if (!state->has_value) return -1;

        size_t length = format_number(state->quotient, data->result);
        if (write_all(file, data->result, length) < 0) return -1;
        *state = (DivisionState){0};
        return 1;
    }

    if (c == ' ' || c == '\t' || c == '\r') return finish_number(state);

    if (c == '+' || c == '-') {
        if (state->token_state != 0) return -1;
        state->sign = c == '-' ? -1 : 1;
        state->number = 0;
        state->token_state = 1;
        return 0;
    }

    if (c < '0' || c > '9') return -1;
    if (state->token_state == 0) {
        state->sign = 1;
        state->number = 0;
    }

    int digit = c - '0';
    long long limit = state->sign < 0 ? (long long)INT_MAX + 1 : INT_MAX;
    if (state->number > (limit - digit) / 10) return -1;

    state->number = state->number * 10 + digit;
    state->token_state = 2;
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 5) return 1;

    int fd = shm_open(argv[2], O_RDWR, 0);
    if (fd < 0) return 1;
    SharedData *data = mmap(NULL, sizeof(SharedData),
                            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return 1;

    sem_t *request = sem_open(argv[3], 0);
    sem_t *reply = sem_open(argv[4], 0);
    if (request == SEM_FAILED || reply == SEM_FAILED) {
        if (request != SEM_FAILED) sem_close(request);
        if (reply != SEM_FAILED) sem_close(reply);
        munmap(data, sizeof(SharedData));
        return 1;
    }

    int file = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file < 0) {
        data->status = -1;
        sem_post(reply);
        sem_close(request);
        sem_close(reply);
        munmap(data, sizeof(SharedData));
        return 1;
    }

    int failed = 0;
    if (sem_post(reply) < 0) failed = 1;
    DivisionState state = {0};

    while (!failed) {
        if (wait_sem(request) < 0) {
            failed = 1;
            break;
        }
        if (data->done) break;

        data->status = 0;
        for (size_t i = 0; i < data->length; i++) {
            int result = process_character(&state, data->line[i], data, file);
            if (result < 0) {
                if (result == -2) {
                    const char message[] = "Division by zero\n";
                    write_all(STDERR_FILENO, message, sizeof(message) - 1);
                } else {
                    const char message[] = "Invalid input\n";
                    write_all(STDERR_FILENO, message, sizeof(message) - 1);
                }
                data->status = -1;
                failed = 1;
                break;
            }
            if (result == 1) data->status = 1;
        }
        if (sem_post(reply) < 0) failed = 1;
    }

    if (close(file) < 0) failed = 1;
    if (sem_close(request) < 0) failed = 1;
    if (sem_close(reply) < 0) failed = 1;
    if (munmap(data, sizeof(SharedData)) < 0) failed = 1;
    return failed ? 1 : 0;
}