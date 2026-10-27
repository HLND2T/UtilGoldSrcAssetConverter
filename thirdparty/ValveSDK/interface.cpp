//=========== (C) Copyright 1999 Valve, L.L.C. All rights reserved. ===========
//
// The copyright to the contents herein is the property of Valve, L.L.C.
// The contents may be used and/or copied only with the written permission of
// Valve, L.L.C., or in accordance with the terms and conditions stipulated in
// the agreement/contract under which the contents have been supplied.
//
// $Header: $
// $NoKeywords: $
//
// Main class factory methods
//=============================================================================

#include <stdio.h>
#include "interface.h"
#include <string.h>
#include <stdlib.h>

#ifndef _WIN32
#    include <dlfcn.h> // dlopen,dlclose, et al
#    include <unistd.h>

#    define HMODULE        void*
#    define GetProcAddress dlsym

#    define _snprintf snprintf
#endif

// ------------------------------------------------------------------------------------ //
// InterfaceReg.
// ------------------------------------------------------------------------------------ //
InterfaceReg* InterfaceReg::s_pInterfaceRegs = NULL;


InterfaceReg::InterfaceReg(InstantiateInterfaceFn fn, const char* pName) :
    m_pName(pName)
{
    m_CreateFn       = fn;
    m_pNext          = s_pInterfaceRegs;
    s_pInterfaceRegs = this;
}

// ------------------------------------------------------------------------------------ //
// CreateInterface.
// This is the primary exported function by a dll, referenced by name via dynamic binding
// that exposes an opqaue function pointer to the interface.
//
// We have the Internal variant so Sys_GetFactoryThis() returns the correct internal
// symbol under GCC/Linux/Mac as CreateInterface is DLL_EXPORT so its global so the loaders
// on those OS's pick exactly 1 of the CreateInterface symbols to be the one that is process wide and
// all Sys_GetFactoryThis() calls find that one, which doesn't work. Using the internal walkthrough here
// makes sure Sys_GetFactoryThis() has the dll specific symbol and GetProcAddress() returns the module specific
// function for CreateInterface again getting the dll specific symbol we need.
// ------------------------------------------------------------------------------------ //
void* CreateInterfaceInternal(const char* pName, int* pReturnCode)
{
    InterfaceReg* pCur;

    for (pCur = InterfaceReg::s_pInterfaceRegs; pCur; pCur = pCur->m_pNext)
    {
        if (strcmp(pCur->m_pName, pName) == 0)
        {
            if (pReturnCode)
            {
                *pReturnCode = IFACE_OK;
            }
            return pCur->m_CreateFn();
        }
    }

    if (pReturnCode)
    {
        *pReturnCode = IFACE_FAILED;
    }
    return NULL;
}

void* CreateInterface(const char* pName, int* pReturnCode)
{
    return CreateInterfaceInternal(pName, pReturnCode);
}


#ifndef _WIN32
// Linux doesn't have this function so this emulates its functionality
//
//
void* GetModuleHandle(const char* name)
{
    void* handle;


    if (name == NULL)
    {
        // hmm, how can this be handled under linux....
        // is it even needed?
        return NULL;
    }

    if ((handle = dlopen(name, RTLD_NOW)) == NULL)
    {
        //printf("Error:%s\n",dlerror());
        // couldn't open this file
        return NULL;
    }

    // read "man dlopen" for details
    // in short dlopen() inc a ref count
    // so dec the ref count by performing the close
    dlclose(handle);
    return handle;
}

#endif

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include "windows.h"
#endif

//-----------------------------------------------------------------------------
// Purpose: returns a pointer to a function, given a module
// Input  : pModuleName - module name
//			*pName - proc name
//-----------------------------------------------------------------------------
static void* Sys_GetProcAddress(const char* pModuleName, const char* pName)
{
    return (void*)GetProcAddress(GetModuleHandle(pModuleName), pName);
}

//-----------------------------------------------------------------------------
// Purpose: Loads a DLL/component from disk and returns a handle to it
// Input  : *pModuleName - filename of the component
// Output : opaque handle to the module (hides system dependency)
//-----------------------------------------------------------------------------
CSysModule* Sys_LoadModule(const char* pModuleName)
{
    // If using the Steam filesystem, either the DLL must be a minimum footprint
    // file in the depot (MFP) or a filesystem GetLocalCopy() call must be made
    // prior to the call to this routine.
#ifdef _WIN32
    HMODULE hDLL = LoadLibrary(pModuleName);
#else
    HMODULE hDLL = NULL;
    char    szAbsoluteModuleName[1024];
    szAbsoluteModuleName[0] = 0;
    if (pModuleName[0] != '/')
    {
        char szCwd[1024];
        char szAbsoluteModuleName[1024];

        getcwd(szCwd, sizeof(szCwd));
        if (szCwd[strlen(szCwd) - 1] == '/')
            szCwd[strlen(szCwd) - 1] = 0;

        _snprintf(szAbsoluteModuleName, sizeof(szAbsoluteModuleName), "%s/%s", szCwd, pModuleName);

        hDLL = dlopen(szAbsoluteModuleName, RTLD_NOW);
    }
    else
    {
        _snprintf(szAbsoluteModuleName, sizeof(szAbsoluteModuleName), "%s", pModuleName);
        hDLL = dlopen(pModuleName, RTLD_NOW);
    }
#endif

    if (!hDLL)
    {
        char str[512];
#ifdef _WIN32
        _snprintf(str, sizeof(str), "%s.dll", pModuleName);
        hDLL = LoadLibrary(str);
#elif defined(__APPLE__)
        _snprintf(str, sizeof(str), "%s.dylib", szAbsoluteModuleName);
        hDLL = dlopen(str, RTLD_NOW);
#else
        _snprintf(str, sizeof(str), "%s.so", szAbsoluteModuleName);
        hDLL = dlopen(str, RTLD_NOW);
#endif
    }

    return reinterpret_cast<CSysModule*>(hDLL);
}

//-----------------------------------------------------------------------------
// Purpose: Unloads a DLL/component from
// Input  : *pModuleName - filename of the component
// Output : opaque handle to the module (hides system dependency)
//-----------------------------------------------------------------------------
void Sys_UnloadModule(CSysModule* pModule)
{
    if (!pModule)
        return;

    HMODULE hDLL = reinterpret_cast<HMODULE>(pModule);

#ifdef _WIN32
    FreeLibrary(hDLL);
#else
    dlclose((void*)hDLL);
#endif
}

//-----------------------------------------------------------------------------
// Purpose: returns a pointer to a function, given a module
// Input  : module - windows HMODULE from Sys_LoadModule()
//			*pName - proc name
// Output : factory for this module
//-----------------------------------------------------------------------------
CreateInterfaceFn Sys_GetFactory(CSysModule* pModule)
{
    if (!pModule)
        return NULL;

    HMODULE hDLL = reinterpret_cast<HMODULE>(pModule);
#ifdef _WIN32
    return reinterpret_cast<CreateInterfaceFn>(GetProcAddress(hDLL, CREATEINTERFACE_PROCNAME));
#else
    // Linux gives this error:
    //../public/interface.cpp: In function `IBaseInterface *(*Sys_GetFactory
    //(CSysModule *)) (const char *, int *)':
    //../public/interface.cpp:154: ISO C++ forbids casting between
    //pointer-to-function and pointer-to-object
    //
    // so lets get around it :)
    return (CreateInterfaceFn)(GetProcAddress(hDLL, CREATEINTERFACE_PROCNAME));
#endif
}

//-----------------------------------------------------------------------------
// Purpose: returns the instance of this module
// Output : interface_instance_t
//-----------------------------------------------------------------------------
CreateInterfaceFn Sys_GetFactoryThis(void)
{
    return &CreateInterfaceInternal;
}

//-----------------------------------------------------------------------------
// Purpose: returns the instance of the named module
// Input  : *pModuleName - name of the module
// Output : interface_instance_t - instance of that module
//-----------------------------------------------------------------------------
CreateInterfaceFn Sys_GetFactory(const char* pModuleName)
{
#ifdef _WIN32
    return reinterpret_cast<CreateInterfaceFn>(Sys_GetProcAddress(pModuleName, CREATEINTERFACE_PROCNAME));
#else
    // see Sys_GetFactory( CSysModule *pModule ) for an explanation
    return (CreateInterfaceFn)(Sys_GetProcAddress(pModuleName, CREATEINTERFACE_PROCNAME));
#endif
}
