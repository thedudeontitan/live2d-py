#include <GL/glew.h>

#include <LAppModel.hpp>
#include <CubismFramework.hpp>
#include <LAppPal.hpp>
#include <LAppAllocator.hpp>
#include <Log.hpp>
#include <unordered_map>
#include <mutex>

#define Py_LIMITED_API
#include <Python.h>

#ifdef WIN32
#include <Windows.h>
#endif

#ifndef Py_IsNone
#define Py_IsNone(o) (o == Py_None)
#endif

static LAppAllocator _cubismAllocator;
static Csm::CubismFramework::Option _cubismOption;

struct PyLAppModelObject
{
    PyObject_HEAD
    LAppModel* model;
    char* lastExpression;
    time_t expStartedAt;
    time_t fadeout;
};

// LAppModel()
static int PyLAppModel_init(PyLAppModelObject* self, PyObject* args, PyObject* kwds)
{
    self->model = new LAppModel();
    self->lastExpression = nullptr;
    self->expStartedAt = -1;
    self->fadeout = -1;
    Info("[M] allocate LAppModel(at=%p)", self->model);
    return 0;
}

static void PyLAppModel_dealloc(PyLAppModelObject* self)
{
    Info("[M] deallocate: PyLAppModelObject(at=%p)", self);
    PyObject_Free(self);
}

// LAppModel->LoadAssets
static PyObject* PyLAppModel_LoadModelJson(PyLAppModelObject* self, PyObject* args)
{
    const char* fileName;
    if (!PyArg_ParseTuple(args, "s", &fileName))
    {
        return NULL;
    }

    self->model->LoadAssets(fileName);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_Resize(PyLAppModelObject* self, PyObject* args)
{
    int ww, wh;
    if (!PyArg_ParseTuple(args, "ii", &ww, &wh))
    {
        PyErr_SetString(PyExc_TypeError, "invalid params.");
        return NULL;
    }

    self->model->Resize(ww, wh);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_Draw(PyLAppModelObject* self, PyObject* args)
{
    self->model->Draw();
    Py_RETURN_NONE;
}

typedef Live2D::Cubism::Framework::ACubismMotion ACubismMotion;

void OnMotionStartedCallback(ACubismMotion* motion)
{
    if (motion->onStartedCallee == nullptr)
    {
        return;
    }
    PyGILState_STATE state = PyGILState_Ensure();
    PyObject* s_call = (PyObject*)motion->onStartedCallee;
    PyObject* result = PyObject_CallFunction(s_call, "si", motion->group.c_str(), motion->no);
    if (result != nullptr)
        Py_XDECREF(result);
    Py_XDECREF(s_call);
    PyGILState_Release(state);
}

void OnMotionFinishedCallback(ACubismMotion* motion)
{
    if (motion->onFinishedCallee == nullptr)
    {
        return;
    }
    PyGILState_STATE state = PyGILState_Ensure();
    PyObject* f_call = (PyObject*)motion->onFinishedCallee;
    PyObject* result = PyObject_CallFunction(f_call, nullptr);
    if (result != nullptr)
        Py_XDECREF(result);
    Py_XDECREF(f_call);
    PyGILState_Release(state);
}

static PyObject* MakeCallee(PyObject* callback)
{
    if (callback == nullptr)
        return nullptr;

    if (Py_IsNone(callback))
    {
        return nullptr;
    }

    if (!PyCallable_Check(callback))
    {
        PyErr_SetString(PyExc_TypeError, "handler must be callable or None");
        return NULL;
    }

    Py_XINCREF(callback);

    return callback;
}

static PyObject* PyLAppModel_StartMotion(PyLAppModelObject* self, PyObject* args, PyObject* kwargs)
{
    const char* group;
    int no, priority;
    PyObject* onStartHandler = nullptr;
    PyObject* onFinishHandler = nullptr;

    static char* kwlist[] = {
        (char*)"group", (char*)"no", (char*)"priority", (char*)"onStartMotionHandler", (char*)"onFinishMotionHandler",
        NULL
    };
    if (!(PyArg_ParseTupleAndKeywords(args, kwargs, "sii|OO", kwlist, &group, &no, &priority, &onStartHandler,
                                      &onFinishHandler)))
    {
        return NULL;
    }


    Csm::CubismMotionQueueEntryHandle _ = self->model->StartMotion(group, no, priority,
                                                                   MakeCallee(onStartHandler),
                                                                   OnMotionStartedCallback,
                                                                   MakeCallee(onFinishHandler),
                                                                   OnMotionFinishedCallback);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_StartRandomMotion(PyLAppModelObject* self, PyObject* args, PyObject* kwargs)
{
    const char* group = nullptr;
    int priority = 3;

    PyObject* onStartHandler = nullptr;
    PyObject* onFinishHandler = nullptr;

    static char* kwlist[] = {
        (char*)"group", (char*)"priority", (char*)"onStartMotionHandler", (char*)"onFinishMotionHandler", NULL
    };
    if (!(PyArg_ParseTupleAndKeywords(args, kwargs, "|siOO", kwlist, &group, &priority, &onStartHandler,
                                      &onFinishHandler)))
    {
        return NULL;
    }

    self->model->StartRandomMotion(group, priority,
                                   MakeCallee(onStartHandler),
                                   OnMotionStartedCallback,
                                   MakeCallee(onFinishHandler),
                                   OnMotionFinishedCallback);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_StopAllMotions(PyLAppModelObject* self, PyObject* args, PyObject* kwargs)
{
    self->model->StopAllMotions();
    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_ResetPose(PyLAppModelObject* self, PyObject* args, PyObject* kwargs)
{
    self->model->ResetPose();
    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_SetExpression(PyLAppModelObject* self, PyObject* args, PyObject* kwargs)
{
    const char* expressionID;
    int fadeout = -1;

    static char* kwlist[] = {
        (char*)"expressionId", (char*)"fadeout", NULL};

    if (!(PyArg_ParseTupleAndKeywords(args, kwargs, "s|i", kwlist, &expressionID, &fadeout)))
    {
        return NULL;
    }

    if (fadeout >= 0)
    {
        auto now = std::chrono::system_clock::now();
        self->expStartedAt = std::chrono::time_point_cast<std::chrono::milliseconds>(now).time_since_epoch().count();
    }
    else
    {
        int len = strlen(expressionID);
        if (self->lastExpression != nullptr)
        {
            delete[] self->lastExpression;
            self->lastExpression = nullptr;
        }
        self->lastExpression = new char[len + 1];
        strcpy(self->lastExpression, expressionID);
        self->lastExpression[len] = '\0';
    }

    self->fadeout = fadeout;
    self->model->SetExpression(expressionID);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_ResetExpression(PyLAppModelObject* self, PyObject* args)
{
    self->fadeout = -1;
    self->expStartedAt = -1;
    if (self->lastExpression != nullptr)
    {
        delete[] self->lastExpression;
        self->lastExpression = nullptr;
    }

    self->model->ResetExpression();
    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_SetRandomExpression(PyLAppModelObject* self, PyObject* args)
{
    self->model->SetRandomExpression();
    Py_RETURN_NONE;
}

typedef Live2D::Cubism::Framework::csmString csmString;

static PyObject* PyLAppModel_HitTest(PyLAppModelObject* self, PyObject* args)
{
    float x, y;
    if (!(PyArg_ParseTuple(args, "ff", &x, &y)))
    {
        return NULL;
    }

    csmString area = self->model->HitTest(x, y);

    return Py_BuildValue("s", area.GetRawString());
}

static PyObject* PyLAppModel_HasMocConsistencyFromFile(PyLAppModelObject* self, PyObject* args)
{
    const char* mocFileName;
    if (!(PyArg_ParseTuple(args, "s", &mocFileName)))
    {
        return NULL;
    }

    bool result = self->model->HasMocConsistencyFromFile(mocFileName);

    if (result)
        Py_RETURN_TRUE;

    Py_RETURN_FALSE;
}

static PyObject* PyLAppModel_Touch(PyLAppModelObject* self, PyObject* args, PyObject* kwargs)
{
    float mx, my;
    PyObject* onStartHandler = nullptr;
    PyObject* onFinishHandler = nullptr;

    static char* kwlist[] = {
        (char*)"mx", (char*)"my", (char*)"onStartMotionHandler", (char*)"onFinishMotionHandler", NULL
    };
    if (!(PyArg_ParseTupleAndKeywords(args, kwargs, "ff|OO", kwlist, &mx, &my, &onStartHandler, &onFinishHandler)))
    {
        return NULL;
    }

    self->model->Touch(mx, my,
                       MakeCallee(onStartHandler),
                       OnMotionStartedCallback,
                       MakeCallee(onFinishHandler),
                       OnMotionFinishedCallback);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_Drag(PyLAppModelObject* self, PyObject* args)
{
    float mx, my;
    if (!(PyArg_ParseTuple(args, "ff", &mx, &my)))
    {
        return NULL;
    }

    self->model->Drag(mx, my);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_IsMotionFinished(PyLAppModelObject* self, PyObject* args)
{
    if (self->model->IsMotionFinished())
    {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject* PyLAppModel_SetOffset(PyLAppModelObject* self, PyObject* args)
{
    float dx, dy;

    if (PyArg_ParseTuple(args, "ff", &dx, &dy) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Missing param 'float dx, float dy'");
        return NULL;
    }

    self->model->SetOffset(dx, dy);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_SetScale(PyLAppModelObject* self, PyObject* args)
{
    float scale;

    if (PyArg_ParseTuple(args, "f", &scale) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Missing param 'float scale'");
        return NULL;
    }

    self->model->SetScale(scale);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_SetParameterValue(PyLAppModelObject* self, PyObject* args)
{
    const char* paramId;
    float value, weight;

    if (PyArg_ParseTuple(args, "sff", &paramId, &value, &weight) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid params (str, float, float)");
        return NULL;
    }

    self->model->SetParameterValue(paramId, value, weight);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_AddParameterValue(PyLAppModelObject* self, PyObject* args)
{
    const char* paramId;
    float value;

    if (PyArg_ParseTuple(args, "sf", &paramId, &value) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid params (str, float)");
        return NULL;
    }

    self->model->AddParameterValue(paramId, value);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_Update(PyLAppModelObject* self, PyObject* args)
{
    if (self->fadeout >= 0)
    {
        auto now = std::chrono::system_clock::now();
        auto value = std::chrono::time_point_cast<std::chrono::milliseconds>(now).time_since_epoch().count();
        time_t elapsed = value - self->expStartedAt;
        if (elapsed >= self->fadeout)
        {
            if (self->lastExpression != nullptr)
            {
                self->model->SetExpression(self->lastExpression);
                Info("reset expression %s", self->lastExpression);
            }
            else
            {
                self->model->ResetExpression();
                Info("clear expression");
            }
            self->fadeout = -1;
        }
    }

    self->model->Update();

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_SetAutoBreathEnable(PyLAppModelObject* self, PyObject* args)
{
    bool enable;

    if (PyArg_ParseTuple(args, "b", &enable) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid param");
        return NULL;
    }

    self->model->SetAutoBreathEnable(enable);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_SetAutoBlinkEnable(PyLAppModelObject* self, PyObject* args)
{
    bool enable;

    if (PyArg_ParseTuple(args, "b", &enable) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid param");
        return NULL;
    }

    self->model->SetAutoBlinkEnable(enable);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_GetParameterCount(PyLAppModelObject* self, PyObject* args)
{
    return PyLong_FromLong(self->model->GetParameterCount());
}

static PyObject* module_live2d_v3_params = nullptr;
static PyObject* typeobject_live2d_v3_parameter = nullptr;

static PyObject* PyLAppModel_GetParameter(PyLAppModelObject* self, PyObject* args)
{
    int index;
    if (PyArg_ParseTuple(args, "i", &index) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid param");
        return NULL;
    }

    const char* id;
    int type;
    float value, maxValue, minValue, defaultValue;
    self->model->GetParameter(index, id, type, value, maxValue, minValue, defaultValue);

    PyObject* instance = PyObject_CallObject(typeobject_live2d_v3_parameter, NULL);
    if (instance == NULL)
    {
        PyErr_Print();
        return NULL;
    }

    PyObject_SetAttrString(instance, "id", PyUnicode_FromString(id));
    PyObject_SetAttrString(instance, "type", PyLong_FromLong(type));
    PyObject_SetAttrString(instance, "value", PyFloat_FromDouble(value));
    PyObject_SetAttrString(instance, "max", PyFloat_FromDouble(maxValue));
    PyObject_SetAttrString(instance, "min", PyFloat_FromDouble(minValue));
    PyObject_SetAttrString(instance, "default", PyFloat_FromDouble(defaultValue));

    return instance;
}

// GetPartCount() -> int
static PyObject* PyLAppModel_GetPartCount(PyLAppModelObject* self, PyObject* args)
{
    return PyLong_FromLong(self->model->GetPartCount());
}

// GetPartId(index: int) -> str
static PyObject* PyLAppModel_GetPartId(PyLAppModelObject* self, PyObject* args)
{
    int index;
    if (PyArg_ParseTuple(args, "i", &index) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid param");
        return NULL;
    }

    return PyUnicode_FromString(self->model->GetPartId(index).GetRawString());
}

// GetPartIds() -> tuple[str]
static PyObject* PyLAppModel_GetPartIds(PyLAppModelObject* self, PyObject* args)
{
    const int size = self->model->GetPartCount();

    PyObject* list = PyList_New(size);

    for (int i = 0; i < size; ++i)
    {
        PyList_SetItem(list, i, PyUnicode_FromString(self->model->GetPartId(i).GetRawString()));
    }

    return list;
}

// SetPartOpacity(id: str, opacity: float) -> None
static PyObject* PyLAppModel_SetPartOpacity(PyLAppModelObject* self, PyObject* args)
{
    int index;
    float opacity;
    if (PyArg_ParseTuple(args, "if", &index, &opacity) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid param");
        return NULL;
    }

    self->model->SetPartOpacity(index, opacity);
    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_HitPart(PyLAppModelObject* self, PyObject* args)
{
    float x, y;
    bool topOnly = false;
    if (PyArg_ParseTuple(args, "ff|b", &x, &y, &topOnly) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid param");
        return NULL;
    }

    PyObject* list = PyList_New(0);
    // std::vector<const char*> list;
    self->model->HitPart(x, y, topOnly, list, [](void* collector, const char* paramId)
    {
        PyList_Append((PyObject*)collector, PyUnicode_FromString(paramId));
        // ((std::vector<const char*>*)collector)->push_back(paramId);
    });

    // PyObject* pylist = PyList_New(list.size());
    // int i = 0;
    // for (auto s: list)
    // {
    // PyList_SetItem(pylist, i, PyUnicode_FromString(s));
    // }

    // return pylist;
    return list;
}

static PyObject* PyLAppModel_SetPartMultiplyColor(PyLAppModelObject* self, PyObject* args)
{
    int index;
    float r, g, b, a;
    if (PyArg_ParseTuple(args, "iffff", &index, &r, &g, &b, &a) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid param");
        return NULL;
    }
    self->model->SetPartMultiplyColor(index, r, g, b, a);
    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_GetPartMultiplyColor(PyLAppModelObject* self, PyObject* args)
{
    int index;
    if (PyArg_ParseTuple(args, "i", &index) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid param");
        return NULL;
    }

    float r, g, b, a;
    self->model->GetPartMultiplyColor(index, r, g, b, a);
    return Py_BuildValue("ffff", r, g, b, a);
}

static PyObject* PyLAppModel_SetPartScreenColor(PyLAppModelObject* self, PyObject* args)
{
    int index;
    float r, g, b, a;
    if (PyArg_ParseTuple(args, "iffff", &index, &r, &g, &b, &a) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid param");
        return NULL;
    }
    self->model->SetPartScreenColor(index, r, g, b, a);

    Py_RETURN_NONE;
}

static PyObject* PyLAppModel_GetPartScreenColor(PyLAppModelObject* self, PyObject* args)
{
    int index;

    if (PyArg_ParseTuple(args, "i", &index) < 0)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid param");
        return NULL;
    }

    float r, g, b, a;
    self->model->GetPartScreenColor(index, r, g, b, a);

    return Py_BuildValue("ffff", r, g, b, a);
}

// 包装模块方法的方法列表
static PyMethodDef PyLAppModel_methods[] = {
    {"LoadModelJson", (PyCFunction)PyLAppModel_LoadModelJson, METH_VARARGS, ""},
    {"Resize", (PyCFunction)PyLAppModel_Resize, METH_VARARGS, ""},
    {"Draw", (PyCFunction)PyLAppModel_Draw, METH_VARARGS, ""},
    {"StartMotion", (PyCFunction)PyLAppModel_StartMotion, METH_VARARGS | METH_KEYWORDS, ""},
    {"StartRandomMotion", (PyCFunction)PyLAppModel_StartRandomMotion, METH_VARARGS | METH_KEYWORDS, ""},
    {"StopAllMotions", (PyCFunction)PyLAppModel_StopAllMotions, METH_VARARGS | METH_KEYWORDS, ""},
    {"ResetPose", (PyCFunction)PyLAppModel_ResetPose, METH_VARARGS | METH_KEYWORDS, ""},

    {"SetExpression", (PyCFunction)PyLAppModel_SetExpression, METH_VARARGS | METH_KEYWORDS, ""},
    {"SetRandomExpression", (PyCFunction)PyLAppModel_SetRandomExpression, METH_VARARGS, ""},
    {"ResetExpression", (PyCFunction)PyLAppModel_ResetExpression, METH_VARARGS, ""},

    {"HitTest", (PyCFunction)PyLAppModel_HitTest, METH_VARARGS, "Get the name of the area being hit."},
    {"HasMocConsistencyFromFile", (PyCFunction)PyLAppModel_HasMocConsistencyFromFile, METH_VARARGS, ""},
    {"Touch", (PyCFunction)PyLAppModel_Touch, METH_VARARGS | METH_KEYWORDS, ""},
    {"Drag", (PyCFunction)PyLAppModel_Drag, METH_VARARGS, ""},
    {"IsMotionFinished", (PyCFunction)PyLAppModel_IsMotionFinished, METH_VARARGS, ""},
    {"SetOffset", (PyCFunction)PyLAppModel_SetOffset, METH_VARARGS, ""},
    {"SetScale", (PyCFunction)PyLAppModel_SetScale, METH_VARARGS, ""},
    {"Update", (PyCFunction)PyLAppModel_Update, METH_VARARGS, ""},

    {"SetAutoBreathEnable", (PyCFunction)PyLAppModel_SetAutoBreathEnable, METH_VARARGS, ""},
    {"SetAutoBlinkEnable", (PyCFunction)PyLAppModel_SetAutoBlinkEnable, METH_VARARGS, ""},

    {"SetParameterValue", (PyCFunction)PyLAppModel_SetParameterValue, METH_VARARGS, ""},
    {"AddParameterValue", (PyCFunction)PyLAppModel_AddParameterValue, METH_VARARGS, ""},
    {"GetParameterCount", (PyCFunction)PyLAppModel_GetParameterCount, METH_VARARGS, ""},
    {"GetParameter", (PyCFunction)PyLAppModel_GetParameter, METH_VARARGS, ""},

    {"GetPartCount", (PyCFunction)PyLAppModel_GetPartCount, METH_VARARGS, ""},
    {"GetPartId", (PyCFunction)PyLAppModel_GetPartId, METH_VARARGS, ""},
    {"GetPartIds", (PyCFunction)PyLAppModel_GetPartIds, METH_VARARGS, ""},
    {"SetPartOpacity", (PyCFunction)PyLAppModel_SetPartOpacity, METH_VARARGS, ""},
    {"HitPart", (PyCFunction)PyLAppModel_HitPart, METH_VARARGS, ""},

    {"SetPartMultiplyColor", (PyCFunction)PyLAppModel_SetPartMultiplyColor, METH_VARARGS, ""},
    {"GetPartMultiplyColor", (PyCFunction)PyLAppModel_GetPartMultiplyColor, METH_VARARGS, ""},

    {"SetPartScreenColor", (PyCFunction)PyLAppModel_SetPartScreenColor, METH_VARARGS, ""},
    {"GetPartScreenColor", (PyCFunction)PyLAppModel_GetPartScreenColor, METH_VARARGS, ""},
    {NULL} // 方法列表结束的标志
};

static PyObject* PyLAppModel_new(PyTypeObject* type, PyObject* args, PyObject* kwds)
{
    PyObject* self = (PyObject*)PyObject_Malloc(sizeof(PyLAppModelObject));
    PyObject_Init(self, type);
    return self;
}

static PyType_Slot PyLAppModel_slots[] = {
    {Py_tp_new, (void*)PyLAppModel_new},
    {Py_tp_init, (void*)PyLAppModel_init},
    {Py_tp_dealloc, (void*)PyLAppModel_dealloc},
    {Py_tp_methods, (void*)PyLAppModel_methods},
    {0, NULL}
};

static PyType_Spec PyLAppModel_spec = {
    "live2d.LAppModel",
    sizeof(PyLAppModelObject),
    0,
    Py_TPFLAGS_DEFAULT,
    PyLAppModel_slots,
};

static PyObject* live2d_init()
{
    _cubismOption.LogFunction = LAppPal::PrintLn;
    _cubismOption.LoggingLevel = Csm::CubismFramework::Option::LogLevel_Verbose;

    Csm::CubismFramework::StartUp(&_cubismAllocator, &_cubismOption);
    Csm::CubismFramework::Initialize();
    Py_RETURN_NONE;
}

static PyObject* live2d_dispose()
{
    Csm::CubismFramework::Dispose();
    Py_RETURN_NONE;
}

static PyObject* live2d_glew_init()
{
    if (glewInit() != GLEW_OK)
    {
        Info("Can't initilize glew.");
    }
    LAppPal::UpdateTime();
    Py_RETURN_NONE;
}

static PyObject* live2d_clear_buffer(PyObject* self, PyObject* args)
{
    // 默认为黑色
    float r = 0.0, g = 0.0, b = 0.0, a = 0.0;

    // 解析传入的参数，允许指定颜色
    if (!PyArg_ParseTuple(args, "|ffff", &r, &g, &b, &a))
    {
        return NULL;
    }

    // 设置清屏颜色
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearDepth(1.0);

    Py_RETURN_NONE;
}

static PyObject* live2d_set_log_enable(PyObject* self, PyObject* args)
{
    bool enable;
    if (!PyArg_ParseTuple(args, "b", &enable))
    {
        PyErr_SetString(PyExc_TypeError, "invalid param");
        return NULL;
    }

    live2dLogEnable = enable;

    Py_RETURN_NONE;
}

static PyObject* live2d_log_enable(PyObject* self, PyObject* args)
{
    if (live2dLogEnable)
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

// 定义live2d模块的方法
static PyMethodDef live2d_methods[] = {
    {"init", (PyCFunction)live2d_init, METH_VARARGS, ""},
    {"dispose", (PyCFunction)live2d_dispose, METH_VARARGS, ""},
    {"glewInit", (PyCFunction)live2d_glew_init, METH_VARARGS, ""},
    {"clearBuffer", (PyCFunction)live2d_clear_buffer, METH_VARARGS, ""},
    {"setLogEnable", (PyCFunction)live2d_set_log_enable, METH_VARARGS, ""},
    {"logEnable", (PyCFunction)live2d_log_enable, METH_VARARGS, ""},
    {NULL, NULL, 0, NULL}
};

// 定义live2d模块
static PyModuleDef liv2d_module = {
    PyModuleDef_HEAD_INIT,
    "live2d",
    "Module that creates live2d objects",
    -1,
    live2d_methods
};

// 模块初始化函数的实现
PyMODINIT_FUNC PyInit_live2d(void)
{
    PyObject* lappmodel_type;

    PyObject* m = PyModule_Create(&liv2d_module);
    if (!m)
    {
        return NULL;
    }

    lappmodel_type = PyType_FromSpec(&PyLAppModel_spec);
    if (!lappmodel_type)
    {
        return NULL;
    }

    if (PyModule_AddObject(m, "LAppModel", lappmodel_type) < 0)
    {
        Py_DECREF(&lappmodel_type);
        Py_DECREF(m);
        return NULL;
    }

    // assume that module `params` is already imported in `live2d/v3/__init__.py`
    module_live2d_v3_params = PyImport_AddModule("live2d.v3.params");
    if (module_live2d_v3_params == NULL)
    {
        PyErr_Print();
        return NULL;
    }


    typeobject_live2d_v3_parameter = PyObject_GetAttrString(module_live2d_v3_params, "Parameter");
    if (typeobject_live2d_v3_parameter == NULL)
    {
        Py_DECREF(module_live2d_v3_params);
        PyErr_Print();
        return NULL;
    }

#ifdef CSM_TARGET_WIN_GL
    // windows 下强制utf-8
    SetConsoleOutputCP(65001);
#endif

    printf("live2d-py (built with Python %s)\n", PY_VERSION);

    return m;
}
