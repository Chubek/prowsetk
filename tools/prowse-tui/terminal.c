#define _GNU_SOURCE
#include "terminal.h"
#include <termlib.h>
#include "termscript_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static DT_TTYSession *tty; static int width=80,height=24;
static void out(const char *s){size_t n=strlen(s),d=0,w=0;DT_Error e;while(tty&&d<n){if(dt_tty_write(tty,s+d,n-d,&w,&e)!=DT_OK||!w)break;d+=w;}}
int pwtk_terminal_open(void){DT_TTYOptions o;DT_Error e;dt_tty_options_init(&o);o.fd=STDIN_FILENO;tty=dt_tty_open(&o,&e);if(!tty)return -1;if(dt_tty_set_raw(tty,&e)!=DT_OK){dt_tty_close(tty);tty=NULL;return -1;}pwtk_terminal_size(&width,&height);out("\033[?25l\033[2J\033[H");return 0;}
void pwtk_terminal_close(void){if(!tty)return;out("\033[0m\033[?25h\033[2J\033[H");dt_tty_close(tty);tty=NULL;}
int pwtk_terminal_size(int*w,int*h){DT_Winsize s;DT_Error e;if(tty&&dt_tty_get_winsize(tty,&s,&e)==DT_OK){width=s.columns;height=s.rows;}if(w)*w=width;if(h)*h=height;return 0;}
void pwtk_terminal_clear(void){out("\033[2J\033[H");}
void pwtk_terminal_line(int row,const char*text,int color,bool inverse){char p[64];snprintf(p,sizeof p,"\033[%d;1H\033[%s;%dm",row+1,inverse?"7":"0",color);out(p);out(text?text:"");out("\033[K");}
void pwtk_terminal_present(void){}
static int rb(unsigned char*b){size_t n=0;DT_Error e;return dt_tty_read(tty,b,1,&n,&e)==DT_OK&&n==1;}
int pwtk_terminal_read(PwtkEvent*ev){unsigned char b,s[8];size_t n=0;if(!ev||!tty)return -1;memset(ev,0,sizeof*ev);if(!rb(&b))return -1;if(b==3){ev->key=PWTK_KEY_CTRL_C;return 0;}if(b==13||b==10){ev->key=PWTK_KEY_ENTER;return 0;}if(b==127||b==8){ev->key=PWTK_KEY_BACKSPACE;return 0;}if(b==27){if(!rb(&b)){ev->key=PWTK_KEY_ESC;return 0;}s[n++]=b;if(b!='['&&b!='O'){ev->key=PWTK_KEY_ESC;return 0;}while(n<sizeof s&&rb(&b)){s[n++]=b;if((b>='A'&&b<='Z')||b=='~')break;}switch(s[n-1]){case'A':ev->key=PWTK_KEY_UP;break;case'B':ev->key=PWTK_KEY_DOWN;break;case'C':ev->key=PWTK_KEY_RIGHT;break;case'D':ev->key=PWTK_KEY_LEFT;break;case'H':ev->key=PWTK_KEY_HOME;break;case'F':ev->key=PWTK_KEY_END;break;case'~':if(n>1&&s[1]=='5')ev->key=PWTK_KEY_PAGE_UP;else if(n>1&&s[1]=='6')ev->key=PWTK_KEY_PAGE_DOWN;break;}return 0;}ev->ch=b;return 0;}
int pwtk_termscript_load_ui(void){TS_Error e;TS_VM *vm=ts_vm_create(&e);if(!vm)return -1;if(ts_vm_register_module(vm,pwtk_termscript_ui_module(),&e)!=TS_OK){ts_vm_free(vm);return -1;}char *output=NULL;TS_Status s=ts_vm_run_file(vm,PWTK_UI_SCRIPT,&output,&e);free(output);ts_vm_free(vm);return s==TS_OK?0:-1;}
