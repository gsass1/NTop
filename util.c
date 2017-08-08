#include "util.h"
#include <stdio.h>
#include <Windows.h>

NORETURN void Die(TCHAR *Fmt, ...)
{
	TCHAR Buffer[1024];
	va_list VaList;

	va_start(VaList, Fmt);
	_vstprintf_s(Buffer, _countof(Buffer), Fmt, VaList);
	va_end(VaList);

	system("cls");
	WriteFile(GetStdHandle(STD_ERROR_HANDLE), Buffer, sizeof(*Buffer) * (_tcslen(Buffer) + 1), NULL, NULL);

	exit(EXIT_FAILURE);
}

/*
 * Instant-fail memory allocators as I believe that helps keep the code clean
 */

void *xmalloc(size_t size)
{
	void *m = malloc(size);

	if(!m)
		Die("malloc");

	return m;
}

void *xrealloc(void *ptr, size_t size)
{
	void *m = realloc(ptr, size);

	if(!m)
		Die("realloc");

	return m;
}

