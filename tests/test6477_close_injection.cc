/* Inject enospc. */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <vector>

#include "backup_test_helpers.h"
#include "backup_internal.h"
#include "real_syscalls.h"

static bool disable_injections = true;
static std::vector<long> injection_pattern; // On the Kth pwrite or write, return ENOSPC, if K is in this vector.
static std::vector<int>  ignore_fds;        // Don't count pwrite or write to any fd in this vector.
static long injection_write_count = 0;

static bool inject_this_time(int fd) {
    if (disable_injections) return false;
    for (size_t i=0; i<ignore_fds.size(); i++) {
        if (ignore_fds[i]==fd) return false;
    }
    long old_count = __sync_fetch_and_add(&injection_write_count,1);
    for (size_t i=0; i<injection_pattern.size(); i++) {
        if (injection_pattern[i]==old_count) {
            return true;
        }
    }
    return false;
}

static close_fun_t original_close;

static int my_close(int fd) {
    fprintf(stderr, "Doing close on fd=%d\n", fd); // ok to do a write, since we aren't further interposing writes in this test.
    if (inject_this_time(fd)) {
        fprintf(stderr, "Injecting error\n");
        errno = EIO;
        return -1;
    } else {
        return original_close(fd);
    }
}

static int expect_error = 0;
static int ercount=0;
static void my_error_fun(int e, const char *s, void *ignore) {
    assert(ignore==NULL);
    ercount++;
    fprintf(stderr, "Got error %d (I expected errno=%d) (%s)\n", e, expect_error, s);
}
    
static char *src;

static void testit(int expect) {
    expect_error = expect;
    disable_injections = true;
    injection_write_count = 0;

    setup_source();
    setup_dirs();
    setup_destination();

    disable_injections = false;

    backup_set_start_copying(false);
    backup_set_keep_capturing(true);

    pthread_t thread;

    int fd = openf(O_RDWR|O_CREAT, 0777, "%s/my.data", src);
    assert(fd>=0);
    fprintf(stderr, "fd=%d\n", fd);
    ignore_fds.push_back(fd);

    start_backup_thread_with_funs(&thread,
                                  get_src(), get_dst(),
                                  simple_poll_fun, NULL,
                                  my_error_fun, NULL,
                                  expect);
    while(!backup_is_capturing()) sched_yield(); // wait for the backup to be capturing.
    fprintf(stderr, "The backup is supposedly capturing\n");
    {
        ssize_t r = pwrite(fd, "hello", 5, 10);
        assert(r==5);
    }
    fprintf(stderr,"About to start copying\n");
    backup_set_start_copying(true);
    {
        int r = close(fd);
        assert(r==0);
    }

    backup_set_keep_capturing(false);
    finish_backup_thread(thread);
}


int test_main(int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    src = get_src();
    original_close = register_close(my_close);

    injection_pattern.push_back(0);
    testit(EIO);
    
    printf("2nd test\n");
    injection_pattern.resize(0);
    injection_pattern.push_back(1);
    testit(0);

    free(src);
    return 0;
}
