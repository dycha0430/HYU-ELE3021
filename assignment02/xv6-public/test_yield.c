#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char * argv[])
{
    int pid = fork();
    if (pid == 0){
        //child
        while(1){
            printf(1, "Child\n");
            yield();
        }
    } else if (pid > 0){
        //parent
        while(1){
            printf(1, "Parent\n");
            yield();
        }
    }

}
