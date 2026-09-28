#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

/* ptr이 NULL이면 msg 출력 후 abort */
#define MUST_NONNULL(ptr, msg)						\
  ((typeof(ptr))__ptr_nonnull_handler((ptr), __FILE__, __LINE__, __func__, (msg)))


/* ret < 0이면 error reporting 후 abort*/
#define MUST_SUCCESS_POSIX(ret)					\
  (__must_success_posix_handler((ret), __FILE__, __LINE__, __func__))

/* assert() 대안 */
#define ASSERT(cond, fmt, ...)                                                 \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "[%s:%d, %s] Assertion failed: " fmt "\n", __FILE__,     \
              __LINE__, __func__, ##__VA_ARGS__);                              \
      abort();                                                                 \
    }                                                                          \
  } while (0)


static inline void *__ptr_nonnull_handler(void *ptr, const char *file, int line,
                                          const char *fn, const char *msg) {
  if (!ptr) {
    fprintf(stderr, "[%s:%d, %s] ptr is null. %s\n", file, line, fn, msg);
    abort();
  }

  return ptr;
}


static inline int __must_success_posix_handler(int ret, const char *file, int line,
                                          const char *fn) {
  if (ret < 0) {
    fprintf(stderr, "[%s:%d, %s] %s(errno=%d).", file, line, fn, strerror(errno), errno);
    abort();
  }
  
  return ret;
}
