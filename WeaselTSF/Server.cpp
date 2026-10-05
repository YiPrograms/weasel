#include "stdafx.h"

#include "Globals.h"
#include "Register.h"
#include "WeaselTSF.h"
#include <VersionHelpers.hpp>

void DllAddRef() {
  InterlockedIncrement(&g_cRefDll);
}

void DllRelease() {
  InterlockedDecrement(&g_cRefDll);
}

class CClassFactory : public IClassFactory {
 public:
  // IUnknown methods
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject);
  STDMETHODIMP_(ULONG) AddRef();
  STDMETHODIMP_(ULONG) Release();

  // IClassFactory methods
  STDMETHODIMP CreateInstance(IUnknown* pUnkOuter,
                              REFIID riid,
                              void** ppvObject);
  STDMETHODIMP LockServer(BOOL fLock);
};

STDMETHODIMP CClassFactory::QueryInterface(REFIID riid, void** ppvObject) {
  if (IsEqualIID(riid, IID_IClassFactory) || IsEqualIID(riid, IID_IUnknown)) {
    *ppvObject = this;
    DllAddRef();
    return NOERROR;
  }
  *ppvObject = NULL;
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CClassFactory::AddRef() {
  DllAddRef();
  return g_cRefDll + 1;
}

STDMETHODIMP_(ULONG) CClassFactory::Release() {
  DllRelease();
  return g_cRefDll + 1;
}

STDMETHODIMP CClassFactory::CreateInstance(IUnknown* pUnkOuter,
                                           REFIID riid,
                                           void** ppvObject) {
  WeaselTSF* pCase;
  HRESULT hr;
  if (ppvObject == NULL)
    return E_INVALIDARG;
  *ppvObject = NULL;
  if (pUnkOuter != NULL)
    return CLASS_E_NOAGGREGATION;
  if ((pCase = new WeaselTSF()) == NULL)
    return E_OUTOFMEMORY;
  hr = pCase->QueryInterface(riid, ppvObject);
  pCase->Release();  // caller still holds ref if hr == S_OK
  return hr;
}

STDMETHODIMP CClassFactory::LockServer(BOOL fLock) {
  if (fLock)
    DllAddRef();
  else
    DllRelease();
  return S_OK;
}

static CClassFactory* g_classFactory = NULL;

static bool IsPerUserRegistration() {
  WCHAR value[2];
  return GetEnvironmentVariableW(L"WEASEL_PER_USER", value, _countof(value)) >
         0;
}

static void BuildGlobalObjects() {
  g_classFactory = new CClassFactory();
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppvObject) {
  if (g_classFactory == NULL) {
    EnterCriticalSection(&g_cs);
    if (g_classFactory == NULL)
      BuildGlobalObjects();
    LeaveCriticalSection(&g_cs);
  }
  if (IsEqualIID(riid, IID_IClassFactory) || IsEqualIID(riid, IID_IUnknown)) {
    *ppvObject = g_classFactory;
    DllAddRef();
    return NOERROR;
  }
  *ppvObject = NULL;
  return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() {
  if (g_cRefDll >= 0)
    return S_FALSE;
  return S_OK;
}

STDAPI DllRegisterServer() {
  if (!RegisterServer())
    return E_FAIL;

  const bool per_user = IsPerUserRegistration();
  const bool registered = per_user
                              ? (RegisterProfilesForCurrentUser() &&
                                 RegisterCategoriesForCurrentUser())
                              : (RegisterProfiles() && RegisterCategories());

  if (!registered) {
    DllUnregisterServer();
    return E_FAIL;
  }
  return S_OK;
}

STDAPI DllUnregisterServer() {
  const bool per_user = IsPerUserRegistration();
  if (per_user) {
    UnregisterProfilesForCurrentUser();
    UnregisterCategoriesForCurrentUser();
  } else {
    UnregisterProfiles();
    UnregisterCategories();
  }

  UnregisterServer();
  return S_OK;
}

STDAPI DllInstall(BOOL install, LPCWSTR command_line) {
  if (!command_line)
    return E_INVALIDARG;

  const std::wstring command(command_line);
  const std::wstring prefix = L"user";
  if (command.compare(0, prefix.size(), prefix) != 0)
    return E_INVALIDARG;

  std::wstring profile;
  if (command.size() > prefix.size() && command[prefix.size()] == L',')
    profile = command.substr(prefix.size() + 1);

  SetEnvironmentVariableW(L"WEASEL_PER_USER", L"1");
  if (!profile.empty())
    SetEnvironmentVariableW(L"TEXTSERVICE_PROFILE", profile.c_str());

  HRESULT result = S_OK;
  if (install) {
    if (!RegisterServer() || !RegisterProfilesForCurrentUser() ||
        !RegisterCategoriesForCurrentUser()) {
      UnregisterProfilesForCurrentUser();
      UnregisterCategoriesForCurrentUser();
      UnregisterServer();
      result = E_FAIL;
    }
  } else {
    UnregisterProfilesForCurrentUser();
    UnregisterCategoriesForCurrentUser();
    UnregisterServer();
  }

  SetEnvironmentVariableW(L"TEXTSERVICE_PROFILE", NULL);
  SetEnvironmentVariableW(L"WEASEL_PER_USER", NULL);
  return result;
}
