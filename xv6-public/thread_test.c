#include "types.h"
#include "stat.h"
#include "user.h"

#define NUM_THREADS 20
#define NUM_INCREMENT 2000000000

int cnt_global = 0;

void* thread_function() {
    int cnt_local = 0;
    for (int i = 0; i < NUM_INCREMENT; ++i){
        cnt_global++;
        cnt_local++;
    }

    printf(1, "local : %d\n", cnt_local);
    thread_exit((void*)cnt_local);

    return 0;
}

int main(int argc, char *argv[]) {
    thread_t th[NUM_THREADS];
    int arg = 2;
    //int ret = 0;
    
    for (int i = 0; i < NUM_THREADS; ++i) {
        if (thread_create(&th[i], thread_function, (void*)arg) != 0) {
            printf(1, "thread_create_error\n");
        }
    }

    void* retval;
    for (int i = 0; i < NUM_THREADS; ++i) {
        thread_join(th[i], &retval);
        printf(1, "Join!! ret : %d\n", retval);
    }
    
    printf(1, "Global : %d\n", cnt_global);

   exit();
}
