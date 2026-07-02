#include <Python.h>
#include <stdint.h>

void flint_py_init(void) {
    Py_Initialize();
}

void flint_py_run(const char* code) {
    PyRun_SimpleString(code);
}

void flint_py_fini(void) {
    Py_Finalize();
}

int64_t flint_py_eval_int(const char* expr) {
    PyObject* main = PyImport_AddModule("__main__");
    PyObject* dict = PyModule_GetDict(main);
    PyObject* result = PyRun_String(expr, Py_eval_input, dict, NULL);
    if (!result) {
        PyErr_Print();
        return 0;
    }
    int64_t val = PyLong_AsLongLong(result);
    Py_DECREF(result);
    return val;
}
