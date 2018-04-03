#ifndef VI_H
#define VI_H

#include <tchar.h>

extern TCHAR *CurrentInputStr;
extern int InInputMode;

typedef struct _KEY_EVENT_RECORD KEY_EVENT_RECORD;

void ViInit(void);
int ViHandleInputKey(KEY_EVENT_RECORD *KeyEvent);
void ViExecInput(void);
void ViEnableInput(void);
void ViDisableInput(void);
void SetViError(TCHAR *Fmt, ...);

#endif
