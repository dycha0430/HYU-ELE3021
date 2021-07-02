#include "types.h"
#include "stat.h"
#include "user.h"

// Test getlev system call
int
main(int argc, char *argv[])
{
    printf(1, "HELLO, %d\n", getlev());
    exit();
}
