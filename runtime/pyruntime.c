#include <Python.h>
#include <stdint.h>

void flint_py_init(void) {
    if (!Py_IsInitialized()) {
        Py_Initialize();
    }
}

void flint_py_run(const char* code) {
    if (!code || !Py_IsInitialized()) return;
    PyRun_SimpleString(code);
}

void flint_py_fini(void) {
    if (Py_IsInitialized()) {
        Py_Finalize();
    }
}

int64_t flint_py_eval_int(const char* expr) {
    if (!expr || !Py_IsInitialized()) return 0;

    PyObject* main = PyImport_AddModule("__main__");   // borrowed ref
    if (!main) { PyErr_Print(); return 0; }
    PyObject* dict = PyModule_GetDict(main);           // borrowed ref
    if (!dict) { PyErr_Print(); return 0; }

    PyObject* result = PyRun_String(expr, Py_eval_input, dict, NULL); // new ref
    if (!result) {
        PyErr_Print();
        return 0;
    }

    int64_t val = PyLong_AsLongLong(result);
    // PyLong_AsLongLong returns -1 and sets an exception on error/overflow;
    // clear it so it doesn't spuriously trip later CPython calls.
    if (val == -1 && PyErr_Occurred()) {
        PyErr_Print();
        Py_DECREF(result);
        return 0;
    }
    Py_DECREF(result);
    return val;
}
