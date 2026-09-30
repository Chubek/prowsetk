#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { PWTK_KEY_NONE=0, PWTK_KEY_CTRL_C, PWTK_KEY_ESC, PWTK_KEY_ENTER, PWTK_KEY_BACKSPACE, PWTK_KEY_UP, PWTK_KEY_DOWN, PWTK_KEY_LEFT, PWTK_KEY_RIGHT, PWTK_KEY_PAGE_UP, PWTK_KEY_PAGE_DOWN, PWTK_KEY_HOME, PWTK_KEY_END, PWTK_KEY_F1, PWTK_KEY_F2 } PwtkKey;
typedef struct { PwtkKey key; unsigned int ch; } PwtkEvent;
int pwtk_terminal_open(void); void pwtk_terminal_close(void); int pwtk_terminal_size(int*, int*); void pwtk_terminal_clear(void); void pwtk_terminal_line(int, const char*, int, bool); void pwtk_terminal_present(void); int pwtk_terminal_read(PwtkEvent*);
int pwtk_termscript_load_ui(void);
#ifdef __cplusplus
}
#endif
