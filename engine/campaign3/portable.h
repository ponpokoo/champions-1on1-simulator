// Compatibility for the unchanged Stage 6c audit drivers on MinGW GCC.
#include <stdlib.h>
#ifdef _WIN32
static inline int campaign3_setenv(const char *k,const char *v,int overwrite) {
    if(!overwrite && getenv(k)) return 0;
    return _putenv_s(k,v);
}
static inline int campaign3_unsetenv(const char *k) { return _putenv_s(k,""); }
#define setenv campaign3_setenv
#define unsetenv campaign3_unsetenv
#endif
#ifdef _WIN32
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
// Freeze binary corpus bytes and LF output independently of CRT text mode.
__attribute__((constructor)) static void campaign3_binary_stdout(void) {
    _setmode(_fileno(stdout), _O_BINARY);
}
#endif
