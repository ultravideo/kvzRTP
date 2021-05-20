#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#else // _WIN32
#ifdef HAVE_GETRANDOM
#include <sys/random.h>
#else // HAVE_GETRANDOM
#include <unistd.h>
#include <sys/syscall.h>
#endif // HAVE_GETRANDOM
#endif // _WIN32

#include <cstdlib>
#include <ctime>

#include "debug.hh"
#include "random.hh"

rtp_error_t uvgrtp::random::init()
{
#ifdef _WIN32
    srand(GetTickCount());
#else
    srand(time(NULL));
#endif
    return RTP_OK;
}

int uvgrtp::random::generate(void *buf, size_t n)
{
#ifdef __linux__
#ifdef HAVE_GETRANDOM
    return getrandom(buf, n, 0);
#else // HAVE_GETRANDOM

    // Replace with the syscall
    int read = syscall(SYS_getrandom, buf, n, 0);

    // On error, return the same value as getrandom()
    if (read == -EINTR || read == -ERESTART) {
        errno = EINTR;
        read = -1;
    }

    if (read < -1) {
        errno = -read;
        read = -1;
    }

    return read;
#endif // HAVE_GETRANDOM
#else // __linux__
    HCRYPTPROV hCryptProv;
    if (CryptAcquireContext(&hCryptProv, NULL, NULL, PROV_RSA_FULL, 0) == TRUE) {
        bool res = CryptGenRandom(hCryptProv, n, (BYTE *)buf);

        CryptReleaseContext(hCryptProv, 0);

        return res ? 0 : -1;
    }
    return -1;
#endif // __linux__
}

uint32_t uvgrtp::random::generate_32()
{
    uint32_t value;

    if (uvgrtp::random::generate(&value, sizeof(uint32_t)) < 0)
        return rand();

    return value;
}

uint64_t uvgrtp::random::generate_64()
{
    uint64_t value;

    if (uvgrtp::random::generate(&value, sizeof(uint64_t)) < 0)
        return rand();

    return value;
}
