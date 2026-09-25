/*
 * Test debug history behavior and the level prefix of debug output.
 *
 * Copyright (C) 2026 The Hamlib Group
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hamlib/rig.h>

#define THREAD_COUNT 8
#define PADDING_SIZE 1536
#define MESSAGE_SIZE 1664
#define HISTORY_LINE_COUNT 25
#define RETAINED_LINE_COUNT 20

struct start_gate
{
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int open;
};

struct worker_context
{
    struct start_gate *gate;
    unsigned int thread;
};

static int ignore_debug_output(enum rig_debug_level_e debug_level,
                               rig_ptr_t arg, const char *fmt, va_list ap)
{
    (void)debug_level;
    (void)arg;
    (void)fmt;
    (void)ap;
    return RIG_OK;
}

static int build_message(char *message, size_t size, unsigned int thread)
{
    char padding[PADDING_SIZE + 1];
    int length;

    memset(padding, (int)('A' + thread), PADDING_SIZE);
    padding[PADDING_SIZE] = '\0';
    length = snprintf(message, size,
                      "concurrent history thread=%u padding=%s marker=%u\n",
                      thread, padding, thread);

    return length >= 0 && (size_t)length < size;
}

static void *worker(void *arg)
{
    struct worker_context *context = arg;
    char message[MESSAGE_SIZE];

    if (!build_message(message, sizeof(message), context->thread))
    {
        abort();
    }

    pthread_mutex_lock(&context->gate->mutex);
    while (!context->gate->open)
    {
        pthread_cond_wait(&context->gate->condition, &context->gate->mutex);
    }
    pthread_mutex_unlock(&context->gate->mutex);

    rig_debug(RIG_DEBUG_TRACE, "%s", message);
    return NULL;
}

static int test_concurrent_history(void)
{
    struct start_gate gate = {
        PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_COND_INITIALIZER,
        0
    };
    struct worker_context contexts[THREAD_COUNT];
    pthread_t threads[THREAD_COUNT];
    size_t expected_length = 0;
    unsigned int thread;

    rig_debug_clear();
    rig_set_debug(RIG_DEBUG_NONE);

    for (thread = 0; thread < THREAD_COUNT; ++thread)
    {
        contexts[thread].gate = &gate;
        contexts[thread].thread = thread;
        if (pthread_create(&threads[thread], NULL, worker,
                           &contexts[thread]) != 0)
        {
            fprintf(stderr, "failed to create worker thread %u\n", thread);
            return 0;
        }
    }

    pthread_mutex_lock(&gate.mutex);
    gate.open = 1;
    pthread_cond_broadcast(&gate.condition);
    pthread_mutex_unlock(&gate.mutex);

    for (thread = 0; thread < THREAD_COUNT; ++thread)
    {
        if (pthread_join(threads[thread], NULL) != 0)
        {
            fprintf(stderr, "failed to join worker thread %u\n", thread);
            return 0;
        }
    }

    pthread_cond_destroy(&gate.condition);
    pthread_mutex_destroy(&gate.mutex);

    for (thread = 0; thread < THREAD_COUNT; ++thread)
    {
        char expected[MESSAGE_SIZE];

        if (!build_message(expected, sizeof(expected), thread))
        {
            return 0;
        }
        expected_length += strlen(expected);
        if (strstr(debugmsgsave, expected) == NULL)
        {
            fprintf(stderr,
                    "history does not contain an intact message "
                    "from thread %u\n",
                    thread);
            return 0;
        }
    }

    if (strlen(debugmsgsave) != expected_length)
    {
        fprintf(stderr, "history retained %zu bytes; expected %zu\n",
                strlen(debugmsgsave), expected_length);
        return 0;
    }

    return 1;
}

static int test_rolling_history(void)
{
    const char *history;
    unsigned int line;

    rig_debug_clear();
    rig_set_debug_callback(ignore_debug_output, NULL);
    rig_set_debug(RIG_DEBUG_TRACE);

    for (line = 0; line < HISTORY_LINE_COUNT; ++line)
    {
        rig_debug(RIG_DEBUG_TRACE, "history line=%u\n", line);
    }

    rig_set_debug_callback(NULL, NULL);
    rig_set_debug(RIG_DEBUG_NONE);

    history = debugmsgsave;
    for (line = HISTORY_LINE_COUNT - RETAINED_LINE_COUNT;
            line < HISTORY_LINE_COUNT; ++line)
    {
        char expected[32];
        int written = snprintf(expected, sizeof(expected),
                               "history line=%u\n", line);

        if (written < 0 || (size_t)written >= sizeof(expected)
                || strncmp(history, expected, (size_t)written) != 0)
        {
            fprintf(stderr, "history did not retain line %u in order\n", line);
            return 0;
        }
        history += written;
    }

    if (*history != '\0')
    {
        fprintf(stderr, "history retained more than %u lines\n",
                RETAINED_LINE_COUNT);
        return 0;
    }

    return 1;
}

/* Reads everything written to a temporary file, from its start */
static int read_all(FILE *file, char *buffer, size_t size)
{
    size_t length;

    fflush(file);
    rewind(file);
    length = fread(buffer, 1, size - 1, file);
    buffer[length] = '\0';
    return ferror(file) == 0;
}

/* Empties a temporary file for the next check */
static int clear_file(FILE *file)
{
    rewind(file);

    if (ftruncate(fileno(file), 0) != 0)
    {
        fprintf(stderr, "failed to empty a temporary file\n");
        return 0;
    }

    return 1;
}

static int test_level_prefix(void)
{
    const char *expected =
        "<2>bug\n<3>err\n<4>warn\n<6>verbose\n<7>trace\n<7>cache\n";
    char output[512];
    FILE *file = tmpfile();

    if (file == NULL)
    {
        fprintf(stderr, "failed to open a temporary file\n");
        return 0;
    }

    rig_set_debug_callback(NULL, NULL);
    rig_set_debug_file(file);
    rig_set_debug(RIG_DEBUG_CACHE);
    rig_set_debug_level_prefix(1);

    rig_debug(RIG_DEBUG_BUG, "bug\n");
    rig_debug(RIG_DEBUG_ERR, "err\n");
    rig_debug(RIG_DEBUG_WARN, "warn\n");
    rig_debug(RIG_DEBUG_VERBOSE, "verbose\n");
    rig_debug(RIG_DEBUG_TRACE, "trace\n");
    rig_debug(RIG_DEBUG_CACHE, "cache\n");

    if (!read_all(file, output, sizeof(output)) || strcmp(output, expected) != 0)
    {
        fprintf(stderr, "level prefixes: got [%s], expected [%s]\n", output, expected);
        fclose(file);
        return 0;
    }

    /* The prefix comes before the time stamp, as sd-daemon(3) wants it first */
    if (!clear_file(file))
    {
        fclose(file);
        return 0;
    }

    rig_set_debug_time_stamp(1);
    rig_debug(RIG_DEBUG_ERR, "stamped\n");
    rig_set_debug_time_stamp(0);

    if (!read_all(file, output, sizeof(output)) || strncmp(output, "<3>", 3) != 0
            || strstr(output, "stamped\n") == NULL || strcmp(output, "<3>stamped\n") == 0)
    {
        fprintf(stderr, "prefix with time stamp: got [%s]\n", output);
        fclose(file);
        return 0;
    }

    /* Without the option, output is as it always was */
    if (!clear_file(file))
    {
        fclose(file);
        return 0;
    }

    rig_set_debug_level_prefix(0);
    rig_debug(RIG_DEBUG_ERR, "plain\n");

    if (!read_all(file, output, sizeof(output)) || strcmp(output, "plain\n") != 0)
    {
        fprintf(stderr, "without the prefix: got [%s]\n", output);
        fclose(file);
        return 0;
    }

    rig_set_debug_file(stderr);
    rig_set_debug(RIG_DEBUG_NONE);
    fclose(file);
    return 1;
}

/* Runs rig_print_error with stderr going to a temporary file, and returns what it wrote */
static int capture_print_error(int prefix, char *output, size_t size)
{
    FILE *file = tmpfile();
    int saved_stderr;
    int ok;

    if (file == NULL)
    {
        return 0;
    }

    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    dup2(fileno(file), STDERR_FILENO);

    rig_set_debug_level_prefix(prefix);
    rig_print_error("rig_open: error = %s\n", "Communication timed out");
    rig_set_debug_level_prefix(0);

    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);

    ok = read_all(file, output, size);
    fclose(file);
    return ok;
}

static int test_print_error(void)
{
    char output[256];

    /* At any debug level, as the daemons print their own errors */
    rig_set_debug(RIG_DEBUG_NONE);

    if (!capture_print_error(1, output, sizeof(output))
            || strcmp(output, "<3>rig_open: error = Communication timed out\n") != 0)
    {
        fprintf(stderr, "error with the prefix: got [%s]\n", output);
        return 0;
    }

    if (!capture_print_error(0, output, sizeof(output))
            || strcmp(output, "rig_open: error = Communication timed out\n") != 0)
    {
        fprintf(stderr, "error without the prefix: got [%s]\n", output);
        return 0;
    }

    return 1;
}

int main(void)
{
    if (!test_concurrent_history() || !test_rolling_history()
            || !test_level_prefix() || !test_print_error())
    {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
