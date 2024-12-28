#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WMI WMI;

WMI* NewWMIService();

int WMIQueryProcessorTime(WMI* wmi, int** loads, size_t* count);

void CloseWMIService(WMI* wmi);

#ifdef __cplusplus
}
#endif
