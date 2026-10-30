// Test-only injected libc lookup. Only the owned getent child calls it.
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int getaddrinfo(const char* host, const char* service, const struct addrinfo* hints,
                struct addrinfo** result) {
    (void)host; (void)service; (void)hints; (void)result;
    const char* path = getenv("ALTMACRO_DNS_FIXTURE_PID");
    if (path) { FILE* f = fopen(path,"w"); if (f) { fprintf(f,"%d",(int)getpid()); fclose(f); } }
    sleep(30);
    return EAI_AGAIN;
}
