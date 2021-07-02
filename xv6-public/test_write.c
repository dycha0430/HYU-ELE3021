#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"
#include "memlayout.h"

int main (int argc, char *argv[]) {
    char* name = "abcd";
    int fd = open("hi", O_CREATE | O_RDWR);
    if (fd < 0) printf(1, "Error open\n");

    if (write(fd, name, sizeof(name)) != sizeof(name)) {
        printf(1, "Error write\n");
    }

    printf(1, "write ok\n");
    sync();
    printf(1, "sync ok\n");
    close(fd);
    exit();
}
