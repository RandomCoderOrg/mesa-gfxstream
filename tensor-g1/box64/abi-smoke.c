#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int
main(void)
{
   uint64_t value = UINT64_C(0x123456789abcdef0);
   value = (value << 7) ^ (value >> 11) ^ UINT64_C(0xa5a5a5a55a5a5a5a);

   printf("ABI_BITS=%zu PID=%ld VALUE=%016" PRIx64 "\n",
          sizeof(void *) * 8, (long)getpid(), value);

   return value == UINT64_C(0xbf8cdf62cb2675c1) ? 0 : 1;
}
