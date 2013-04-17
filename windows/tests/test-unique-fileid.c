#include <stdio.h>
#include <stdlib.h>
#include <toku_assert.h>
#include <fcntl.h>
#include <test.h>

int test_main(int argc, char *argv[]) {
    int r;
    int fd;
    struct fileid fid;

    fd = open(DEV_NULL_FILE, O_RDWR);
    assert(fd != -1);

    r = toku_os_get_unique_file_id(fd, &fid);
    printf("%s:%d %d\n", __FILE__, __LINE__, r);

    r = close(fd);
    assert(r != -1);
    return 0;
}
