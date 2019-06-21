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

#ifndef VI_H
#define VI_H

#include <tchar.h>

extern TCHAR *CurrentInputStr;
extern int InInputMode;

typedef struct _KEY_EVENT_RECORD KEY_EVENT_RECORD;

void ViInit(void);
int ViHandleInputKey(KEY_EVENT_RECORD *KeyEvent);
void ViExecInput(void);
void ViEnableInput(TCHAR InitialKey);
void ViDisableInput(void);
void SetViError(TCHAR *Fmt, ...);

#endif
