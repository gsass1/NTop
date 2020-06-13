/* 
 * NTop - an htop clone for Windows
 * Copyright (c) 2019 Gian Sass
 * 
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

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

	WriteFile(GetStdHandle(STD_ERROR_HANDLE), Buffer, (DWORD)(sizeof(*Buffer) * (_tcslen(Buffer) + 1)), 0, 0);
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
