// TweakInject.c - the tweak loader for the iPhone 5s (iOS 12, Chimera).
//
// Chimera's injector (pspawn_payload) loads /usr/lib/TweakInject.dylib into every process it starts. This is our own,
// tiny version of that file. It does one thing: inside SpringBoard, and nowhere else, it loads /usr/lib/FBMode12.dylib.
// In every other process the constructor returns after one string compare, so it cannot break them.
//
// Off switches (create the file as root, or use the Chimera app: Enable Tweaks off):
//   /.disable_tweakinject   Chimera does not inject anything.
//   /var/root/.disable_fbmode   this loader does nothing.
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor)) static void fb_loader(void) {
    char path[1024];
    uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n) != 0) return;
    static const char tail[] = "/SpringBoard.app/SpringBoard";
    size_t l = strlen(path), t = sizeof tail - 1;
    if (l < t || strcmp(path + l - t, tail) != 0) return;          // not SpringBoard: leave at once
    if (access("/var/root/.disable_fbmode", F_OK) == 0) return;
    dlopen("/usr/lib/FBMode12.dylib", RTLD_NOW);
}
