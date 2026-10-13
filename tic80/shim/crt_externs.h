// crt_externs.h - the iOS SDK hides it. The Janet sources only need the environment of the process from it.
#ifndef SHIM_CRT_EXTERNS_H
#define SHIM_CRT_EXTERNS_H
extern char **environ;
static inline char ***_NSGetEnviron(void) { return &environ; }
#endif
