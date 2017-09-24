#include "util.h"
#include <stdio.h>
#include <windows.h>

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

extern HANDLE ConsoleHandle;

/*
 * Instant-fail memory allocators as I believe that helps keep the code clean
 */

void *xmalloc(size_t size)
{
	void *m = malloc(size);

	if(!m)
		Die(_T("malloc"));

	return m;
}

void *xrealloc(void *ptr, size_t size)
{
	void *m = realloc(ptr, size);

	if(!m)
		Die(_T("realloc"));

	return m;
}

void *xcalloc(size_t num, size_t size)
{
	void *m = calloc(num, size);

	if(!m)
		Die(_T("calloc"));

	return m;
}
