#ifndef SURA_ASSERT_H
#define SURA_ASSERT_H

void sura_assert_fail(const char *expr, const char *file, int line);

#define assert(x) ((x) ? (void)0 : sura_assert_fail(#x, __FILE__, __LINE__))

#endif
