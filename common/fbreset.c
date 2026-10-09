// fbreset.c - take our surface off display layer 1, so a stuck overlay picture goes away. (iOS)
// Run it after a program was killed hard (SIGKILL) or crashed and left its last picture on screen.
#include "../fb.h"
#include <stdlib.h>
int main(void) {
    FB f;
    if (fb_init(&f, 640, 960)) return 1;
    f.layer = 1;
    fb_unset(&f);
    unlink("/tmp/fbmode");
    printf("overlay layer 1 cleared\n");
    return 0;
}
