// fbls.c - list a folder for the Phone Files app: one line per entry, tab separated: type size mtime name.
// type is d (folder), f (file), l (link) or ? . A tiny program because the Android shell on old phones has no find and a
// different ls on every version. usage: fbls FOLDER
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
static __thread char tls_pad[32] __attribute__((aligned(32)));
int main(int argc, char **argv) {
    tls_pad[0] = 1;
    if (argc < 2) return 2;
    DIR *d = opendir(argv[1]);
    if (!d) { perror(argv[1]); return 1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        char p[4096]; snprintf(p, sizeof p, "%s/%s", argv[1], e->d_name);
        struct stat st, ls;
        if (lstat(p, &ls)) continue;
        char t = S_ISLNK(ls.st_mode) ? 'l' : '?';
        if (!stat(p, &st)) t = S_ISDIR(st.st_mode) ? 'd' : (S_ISREG(st.st_mode) ? 'f' : (t == 'l' ? 'l' : '?'));
        else st = ls;
        printf("%c\t%lld\t%ld\t%s\n", t, (long long)st.st_size, (long)st.st_mtime, e->d_name);
    }
    closedir(d);
    return 0;
}
