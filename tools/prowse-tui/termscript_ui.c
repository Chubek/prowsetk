#include "termscript_ui.h"
#include <string.h>
static TS_Status accent(TS_VM *vm, void *userdata, const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *error) {
    (void)vm; (void)userdata; if (ts_check_argc(vm, argv, argc, 1, error) != TS_OK || argv[0].type != TS_STRING) return TS_ERR_INVAL;
    return ts_value_make_string(ret, argv[0].as.string);
}
static const TS_FuncDef funcs[] = {{"accent", accent, NULL}, {NULL, NULL, NULL}};
static const TS_Module module = {"pwtk", funcs};
const TS_Module *pwtk_termscript_ui_module(void) { return &module; }
