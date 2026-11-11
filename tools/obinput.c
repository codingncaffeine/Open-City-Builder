/* tools/obinput.c: test-bed input for the original program, run in its Wine prefix by tools/original-session.sh (OB_ACTIONS).
   Build: i686-w64-mingw32-gcc -O2 -o scratch/obinput.exe tools/obinput.c -luser32
   obinput click X Y | rclick X Y | drag X0 Y0 X1 Y1 | rdrag X0 Y0 X1 Y1 | move X Y | wheel N | key VK | info
   Coordinates are client coordinates of the game's main window (the largest visible top-level window). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static HWND best; static long bestArea;
static BOOL CALLBACK enumw(HWND h, LPARAM l){ RECT r; if(!IsWindowVisible(h)) return TRUE; GetWindowRect(h,&r);
  long a=(r.right-r.left)*(r.bottom-r.top); if(a>bestArea){bestArea=a;best=h;} return TRUE; }
static void at(int x,int y){ POINT p={x,y}; ClientToScreen(best,&p); SetCursorPos(p.x,p.y); }
static void mouse(DWORD f, int data){ INPUT i={0}; i.type=INPUT_MOUSE; i.mi.dwFlags=f; i.mi.mouseData=data; SendInput(1,&i,sizeof i); }
int main(int argc,char**argv){
  if(argc<2) return 2;
  EnumWindows(enumw,0); if(!best){ fprintf(stderr,"no window\n"); return 1; }
  SetForegroundWindow(best);
  if(!strcmp(argv[1],"click")||!strcmp(argv[1],"rclick")){ int r=argv[1][0]=='r';
    at(atoi(argv[2]),atoi(argv[3])); Sleep(80);
    mouse(r?MOUSEEVENTF_RIGHTDOWN:MOUSEEVENTF_LEFTDOWN,0); Sleep(60); mouse(r?MOUSEEVENTF_RIGHTUP:MOUSEEVENTF_LEFTUP,0); }
  else if(!strcmp(argv[1],"drag")||!strcmp(argv[1],"rdrag")){ int r=argv[1][0]=='r';
    int x0=atoi(argv[2]),y0=atoi(argv[3]),x1=atoi(argv[4]),y1=atoi(argv[5]),k;
    at(x0,y0); Sleep(80); mouse(r?MOUSEEVENTF_RIGHTDOWN:MOUSEEVENTF_LEFTDOWN,0);
    for(k=1;k<=30;k++){ Sleep(30); at(x0+(x1-x0)*k/30,y0+(y1-y0)*k/30); }
    Sleep(100); mouse(r?MOUSEEVENTF_RIGHTUP:MOUSEEVENTF_LEFTUP,0); }
  else if(!strcmp(argv[1],"move")) at(atoi(argv[2]),atoi(argv[3]));
  else if(!strcmp(argv[1],"wheel")) mouse(MOUSEEVENTF_WHEEL, atoi(argv[2])*WHEEL_DELTA);
  else if(!strcmp(argv[1],"key")){ INPUT i={0}; i.type=INPUT_KEYBOARD; i.ki.wVk=(WORD)strtol(argv[2],0,0); SendInput(1,&i,sizeof i);
    Sleep(60); i.ki.dwFlags=KEYEVENTF_KEYUP; SendInput(1,&i,sizeof i); }
  else if(!strcmp(argv[1],"info")){ RECT r; GetClientRect(best,&r); char t[256]; GetWindowTextA(best,t,256); printf("%p '%s' %ldx%ld\n",best,t,r.right,r.bottom); }
  return 0; }
