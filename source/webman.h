/* webMAN MOD on this console, over HTTP to 127.0.0.1:80, from a thread of its own so that nothing else
   ever waits for it (after TEE PS3 Live's notify.c):
     - messages on the TV through /popup.ps3?text; a newer one replaces one still waiting
     - the CPU and RSX temperatures and the fan speed from its status page /cpursx.ps3, every 5 s while
       someone wants them
   Only these two pages are ever asked for - the status page without any of its fan settings - so
   nothing on the console changes. Without webMAN the requests just fail. */
#ifndef WEBMAN_H
#define WEBMAN_H

int  webman_start(void);
void webman_stop(void);

void webman_popup(const char *text);

/* 1: ask for the temperatures every 5 s, 0: stop asking */
void webman_watch_temps(int on);
/* the latest temperatures in degrees Celsius and the fan speed in % (rsx and fan -1 if webMAN gave
   none); 0 if webMAN has not answered in the last 15 s */
int  webman_temps(int *cpu, int *rsx, int *fan);

#endif
