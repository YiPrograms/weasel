#include "stdafx.h"
#include <cstdarg>
#include "Register.h"
#include <strsafe.h>
#include <WeaselUtility.h>

#define CLSID_STRLEN 38  // strlen("{xxxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxx}")

static const char c_szInfoKeyPrefix[] = "CLSID\\";
static const char c_szTipKeyPrefix[] = "Software\\Microsft\\CTF\\TIP\\";
static const char c_szInProcSvr32[] = "InprocServer32";
static const char c_szModelName[] = "ThreadingModel";
static const char c_szUserClassesRoot[] = "Software\\Classes";
static const WCHAR c_szUserTipRoot[] = L"Software\\Microsoft\\CTF\\TIP";
static const WCHAR c_szUserCategoryRoot[] =
    L"Software\\Microsoft\\CTF\\Categories\\Category\\Item";

static void TraceRegistration(const wchar_t* format, ...) {
  WCHAR path[MAX_PATH] = {};
  if (!GetTempPathW(ARRAYSIZE(path), path))
    return;
  if (FAILED(StringCchCatW(path, ARRAYSIZE(path), L"weasel-register.log")))
    return;

  WCHAR message[1024] = {};
  va_list args;
  va_start(args, format);
  StringCchVPrintfW(message, ARRAYSIZE(message), format, args);
  va_end(args);

  HANDLE file =
      CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE)
    return;

  char utf8[4096] = {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8,
                                         ARRAYSIZE(utf8), NULL, NULL);
  if (length > 1) {
    DWORD written;
    WriteFile(file, utf8, length - 1, &written, NULL);
    static const char newline[] = "\r\n";
    WriteFile(file, newline, sizeof(newline) - 1, &written, NULL);
  }
  CloseHandle(file);
}

static bool IsPerUserRegistration() {
  WCHAR value[2];
  return GetEnvironmentVariableW(L"WEASEL_PER_USER", value, _countof(value)) >
         0;
}

static bool OverrideMachineRegistryForCurrentUser(bool enable) {
  if (!enable) {
    const LSTATUS status = RegOverridePredefKey(HKEY_LOCAL_MACHINE, NULL);
    TraceRegistration(L"Restore HKLM override status=%ld", status);
    return status == ERROR_SUCCESS;
  }

  HKEY current_user = NULL;
  const LSTATUS open_status =
      RegOpenCurrentUser(KEY_READ | KEY_WRITE, &current_user);
  TraceRegistration(L"RegOpenCurrentUser status=%ld", open_status);
  if (open_status != ERROR_SUCCESS)
    return false;

  const LSTATUS override_status =
      RegOverridePredefKey(HKEY_LOCAL_MACHINE, current_user);
  RegCloseKey(current_user);
  TraceRegistration(L"Override HKLM to HKCU status=%ld", override_status);
  return override_status == ERROR_SUCCESS;
}

static BOOL OpenClassesRoot(HKEY* root, bool* close_root) {
  if (!IsPerUserRegistration()) {
    *root = HKEY_CLASSES_ROOT;
    *close_root = false;
    return TRUE;
  }

  DWORD disposition;
  *close_root = true;
  return RegCreateKeyExA(HKEY_CURRENT_USER, c_szUserClassesRoot, 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, root,
                         &disposition) == ERROR_SUCCESS;
}

HKL FindIME(LANGID langid) {
  HKL hKL = NULL;
  WCHAR key[9];
  HKEY hKey;
  LSTATUS ret =
      RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts", 0,
                    KEY_READ, &hKey);
  if (ret == ERROR_SUCCESS) {
    for (DWORD id = (0xE0200000 | langid);
         hKL == NULL && id <= (0xE0FF0000 | langid); id += 0x10000) {
      StringCchPrintfW(key, _countof(key), L"%08X", id);
      HKEY hSubKey;
      ret = RegOpenKeyExW(hKey, key, 0, KEY_READ, &hSubKey);
      if (ret == ERROR_SUCCESS) {
        WCHAR data[32];
        DWORD type;
        DWORD size = sizeof data;
        ret = RegQueryValueExW(hSubKey, L"Ime File", NULL, &type, (LPBYTE)data,
                               &size);
        if (ret == ERROR_SUCCESS && type == REG_SZ &&
            _wcsicmp(data, L"weasel.ime") == 0)
          hKL = (HKL)id;
      }
      RegCloseKey(hSubKey);
    }
  }
  RegCloseKey(hKey);
  return hKL;
}

BOOL RegisterProfiles() {
#define CHECK_HR(hr) \
  if (FAILED(hr))    \
  return FALSE

  CComPtr<ITfInputProcessorProfileMgr> pInputProcessorProfileMgr;
  CHECK_HR(pInputProcessorProfileMgr.CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_ALL));
  WCHAR szProfile[100];
  std::wstring profile{};
  DWORD dwSize = GetEnvironmentVariable(L"TEXTSERVICE_PROFILE", szProfile,
                                        ARRAYSIZE(szProfile));
  if (dwSize > 0) {
    profile = szProfile;
  }
  BOOL hansEnable = (profile == L"hans");
  BOOL hantEnable = (profile == L"hant");
  BOOL hkEnable = (profile == L"hongkong");
  BOOL macauEnable = (profile == L"macau");
  BOOL sgEnable = (profile == L"singapore");
  // fallback hans enable
  hansEnable = hansEnable || (!hantEnable && !hansEnable && !hkEnable &&
                              !macauEnable && !sgEnable);

  const auto text_service_desc = get_weasel_ime_name();
  const WCHAR* text_service_desc_str = text_service_desc.c_str();
  ULONG text_service_desc_len = text_service_desc.size() * sizeof(wchar_t);

  WCHAR achIconFile[MAX_PATH];
  ULONG cchIconFile =
      GetModuleFileNameW(g_hInst, achIconFile, ARRAYSIZE(achIconFile));

  const auto register_profile = [&](LANGID langId, HKL hkl, BOOL enable) {
    return pInputProcessorProfileMgr->RegisterProfile(
        c_clsidTextService, langId, c_guidProfile, text_service_desc_str,
        text_service_desc_len, achIconFile, cchIconFile, TEXTSERVICE_ICON_INDEX,
        hkl, 0, enable, 0);
  };

  const auto hkl_hans = FindIME(TEXTSERVICE_LANGID_HANS);
  const auto hkl_hant = FindIME(TEXTSERVICE_LANGID_HANT);
  CHECK_HR(register_profile(TEXTSERVICE_LANGID_HANS, hkl_hans, hansEnable));
  CHECK_HR(register_profile(TEXTSERVICE_LANGID_HANT, hkl_hant, hantEnable));
  CHECK_HR(register_profile(TEXTSERVICE_LANGID_HONGKONG, NULL, hkEnable));
  CHECK_HR(register_profile(TEXTSERVICE_LANGID_MACAU, NULL, macauEnable));
  CHECK_HR(register_profile(TEXTSERVICE_LANGID_SINGAPORE, NULL, sgEnable));
#undef CHECK_HR

  return TRUE;
}

static bool GuidToStringW(REFGUID guid, std::wstring& value) {
  WCHAR buffer[64];
  if (StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) <= 0)
    return false;
  value = buffer;
  return true;
}

BOOL RegisterProfilesForCurrentUser() {
  WCHAR selected_profile[100] = {};
  std::wstring profile;
  if (GetEnvironmentVariableW(L"TEXTSERVICE_PROFILE", selected_profile,
                              ARRAYSIZE(selected_profile)) > 0) {
    profile = selected_profile;
  }

  BOOL hans_enable = (profile == L"hans");
  BOOL hant_enable = (profile == L"hant");
  BOOL hk_enable = (profile == L"hongkong");
  BOOL macau_enable = (profile == L"macau");
  BOOL sg_enable = (profile == L"singapore");
  hans_enable = hans_enable || (!hant_enable && !hans_enable && !hk_enable &&
                                !macau_enable && !sg_enable);

  WCHAR icon_file[MAX_PATH] = {};
  const ULONG icon_file_len =
      GetModuleFileNameW(g_hInst, icon_file, ARRAYSIZE(icon_file));
  const auto description = get_weasel_ime_name();

  if (!OverrideMachineRegistryForCurrentUser(true))
    return FALSE;

  bool ok = true;
  CComPtr<ITfInputProcessorProfiles> input_processor_profiles;
  HRESULT hr = input_processor_profiles.CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER);
  TraceRegistration(L"Per-user CoCreate ITfInputProcessorProfiles hr=0x%08X",
                    hr);
  if (FAILED(hr)) {
    ok = false;
  } else {
    hr = input_processor_profiles->Register(c_clsidTextService);
    TraceRegistration(L"Per-user Register text service hr=0x%08X", hr);
    ok = SUCCEEDED(hr);
  }

  CComPtr<ITfInputProcessorProfileMgr> profile_mgr;
  if (ok) {
    hr = profile_mgr.CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL,
                                      CLSCTX_INPROC_SERVER);
    TraceRegistration(L"Per-user CoCreate ProfileMgr hr=0x%08X", hr);
    ok = SUCCEEDED(hr);
  }

  const struct {
    LANGID lang_id;
    BOOL enable;
  } profiles[] = {
      {TEXTSERVICE_LANGID_HANS, hans_enable},
      {TEXTSERVICE_LANGID_HANT, hant_enable},
      {TEXTSERVICE_LANGID_HONGKONG, hk_enable},
      {TEXTSERVICE_LANGID_MACAU, macau_enable},
      {TEXTSERVICE_LANGID_SINGAPORE, sg_enable},
  };

  if (ok) {
    for (const auto& item : profiles) {
      hr = profile_mgr->RegisterProfile(
          c_clsidTextService, item.lang_id, c_guidProfile, description.c_str(),
          static_cast<ULONG>(description.size()), icon_file, icon_file_len,
          TEXTSERVICE_ICON_INDEX, NULL, 0, item.enable, 0);
      TraceRegistration(
          L"Per-user RegisterProfile lang=0x%04X enable=%d "
          L"hr=0x%08X",
          item.lang_id, item.enable, hr);
      if (FAILED(hr)) {
        ok = false;
        break;
      }
    }
  }

  if (!ok) {
    if (profile_mgr) {
      for (const auto& item : profiles) {
        profile_mgr->UnregisterProfile(c_clsidTextService, item.lang_id,
                                       c_guidProfile, 0);
      }
    }
    if (input_processor_profiles)
      input_processor_profiles->Unregister(c_clsidTextService);
  }

  const bool restored = OverrideMachineRegistryForCurrentUser(false);
  return ok && restored;
}

void UnregisterProfilesForCurrentUser() {
  if (OverrideMachineRegistryForCurrentUser(true)) {
    UnregisterProfiles();
    OverrideMachineRegistryForCurrentUser(false);
  }

  // Clean up registry-only registrations created by older per-user builds.
  std::wstring clsid;
  if (!GuidToStringW(c_clsidTextService, clsid))
    return;
  const std::wstring tip_root = std::wstring(c_szUserTipRoot) + L"\\" + clsid;
  RegDeleteTreeW(HKEY_CURRENT_USER, tip_root.c_str());
}

void UnregisterProfiles() {
  CComPtr<ITfInputProcessorProfileMgr> pInputProcessorProfileMgr;
  if (FAILED(pInputProcessorProfileMgr.CoCreateInstance(
          CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_ALL)))
    return;
  const auto unregister_profile = [&](LANGID id) {
    pInputProcessorProfileMgr->UnregisterProfile(c_clsidTextService, id,
                                                 c_guidProfile, 0);
  };
  unregister_profile(TEXTSERVICE_LANGID_HANS);
  unregister_profile(TEXTSERVICE_LANGID_HANT);
  unregister_profile(TEXTSERVICE_LANGID_HONGKONG);
  unregister_profile(TEXTSERVICE_LANGID_MACAU);
  unregister_profile(TEXTSERVICE_LANGID_SINGAPORE);
}

const GUID SupportCategories0[] = {
    GUID_TFCAT_CATEGORY_OF_TIP, GUID_TFCAT_TIP_KEYBOARD,
    // GUID_TFCAT_TIP_SPEECH,
    // GUID_TFCAT_TIP_HANDWRITING,
    GUID_TFCAT_TIPCAP_SECUREMODE, GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT, GUID_TFCAT_TIPCAP_COMLESS,
    GUID_TFCAT_TIPCAP_WOW16, GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT, GUID_TFCAT_PROP_AUDIODATA,
    GUID_TFCAT_PROP_INKDATA, GUID_TFCAT_PROPSTYLE_CUSTOM,
    GUID_TFCAT_PROPSTYLE_STATIC, GUID_TFCAT_PROPSTYLE_STATICCOMPACT,
    GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, GUID_TFCAT_DISPLAYATTRIBUTEPROPERTY};

BOOL RegisterCategories() {
  CComPtr<ITfCategoryMgr> pCategoryMgr = NULL;
  if (FAILED(CoCreateInstance(CLSID_TF_CategoryMgr, NULL, CLSCTX_INPROC_SERVER,
                              IID_ITfCategoryMgr, (LPVOID*)&pCategoryMgr)))
    return FALSE;
  for (const auto& guid : SupportCategories0) {
    if (FAILED(pCategoryMgr->RegisterCategory(c_clsidTextService, guid,
                                              c_clsidTextService)))
      return FALSE;
  }
  return TRUE;
}

BOOL RegisterCategoriesForCurrentUser() {
  if (!OverrideMachineRegistryForCurrentUser(true))
    return FALSE;

  const bool registered = RegisterCategories();
  TraceRegistration(L"Per-user RegisterCategories=%d", registered);
  if (!registered)
    UnregisterCategories();

  const bool restored = OverrideMachineRegistryForCurrentUser(false);
  return registered && restored;
}

void UnregisterCategoriesForCurrentUser() {
  if (OverrideMachineRegistryForCurrentUser(true)) {
    UnregisterCategories();

    CComPtr<ITfInputProcessorProfiles> input_processor_profiles;
    const HRESULT create_hr = input_processor_profiles.CoCreateInstance(
        CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER);
    TraceRegistration(L"Per-user uninstall CoCreate profiles hr=0x%08X",
                      create_hr);
    if (SUCCEEDED(create_hr)) {
      const HRESULT unregister_hr =
          input_processor_profiles->Unregister(c_clsidTextService);
      TraceRegistration(L"Per-user Unregister text service hr=0x%08X",
                        unregister_hr);
    }

    OverrideMachineRegistryForCurrentUser(false);
  }

  // Clean up registry-only category entries created by older per-user builds.
  std::wstring clsid;
  if (!GuidToStringW(c_clsidTextService, clsid))
    return;

  for (const auto& guid : SupportCategories0) {
    std::wstring category;
    if (!GuidToStringW(guid, category))
      continue;
    const std::wstring path =
        std::wstring(c_szUserCategoryRoot) + L"\\" + category + L"\\" + clsid;
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  }
}

void UnregisterCategories() {
  CComPtr<ITfCategoryMgr> pCategoryMgr = NULL;
  if (FAILED(CoCreateInstance(CLSID_TF_CategoryMgr, NULL, CLSCTX_INPROC_SERVER,
                              IID_ITfCategoryMgr, (LPVOID*)&pCategoryMgr)))
    return;
  for (const auto& guid : SupportCategories0)
    pCategoryMgr->UnregisterCategory(c_clsidTextService, guid,
                                     c_clsidTextService);
}

static BOOL CLSIDToStringA(REFGUID refGUID, char* pchA) {
  static const BYTE GuidMap[] = {3,   2, 1, 0,   '-', 5,  4,  '-', 7,  6,
                                 '-', 8, 9, '-', 10,  11, 12, 13,  14, 15};

  static const char szDigits[] = "0123456789ABCDEF";

  int i;
  char* p = pchA;

  const BYTE* pBytes = (const BYTE*)&refGUID;

  *p++ = '{';
  for (i = 0; i < sizeof(GuidMap); i++) {
    if (GuidMap[i] == '-')
      *p++ = '-';
    else {
      *p++ = szDigits[(pBytes[GuidMap[i]] & 0xF0) >> 4];
      *p++ = szDigits[(pBytes[GuidMap[i]] & 0x0F)];
    }
  }
  *p++ = '}';
  *p = '\0';
  return TRUE;
}

static LONG RecurseDeleteKeyA(HKEY hParentKey, LPCSTR lpszKey) {
  HKEY hKey;
  LONG lRes;
  FILETIME time;
  CHAR szBuffer[256];
  DWORD dwSize = ARRAYSIZE(szBuffer);

  if (RegOpenKeyA(hParentKey, lpszKey, &hKey) != ERROR_SUCCESS)
    return ERROR_SUCCESS;

  lRes = ERROR_SUCCESS;
  while (RegEnumKeyExA(hKey, 0, szBuffer, &dwSize, NULL, NULL, NULL, &time) ==
         ERROR_SUCCESS) {
    szBuffer[ARRAYSIZE(szBuffer) - 1] = '\0';
    lRes = RecurseDeleteKeyA(hKey, szBuffer);
    if (lRes != ERROR_SUCCESS)
      break;
    dwSize = ARRAYSIZE(szBuffer);
  }
  RegCloseKey(hKey);

  return lRes == ERROR_SUCCESS ? RegDeleteKeyA(hParentKey, lpszKey) : lRes;
}

BOOL RegisterServer() {
  DWORD dw;
  HKEY hClassesRoot;
  HKEY hKey;
  HKEY hSubKey;
  BOOL fRet;
  bool closeClassesRoot;
  char achIMEKey[ARRAYSIZE(c_szInfoKeyPrefix) + CLSID_STRLEN];
  char achFileName[MAX_PATH];

  if (!CLSIDToStringA(c_clsidTextService,
                      achIMEKey + ARRAYSIZE(c_szInfoKeyPrefix) - 1))
    return FALSE;
  memcpy(achIMEKey, c_szInfoKeyPrefix, sizeof(c_szInfoKeyPrefix) - 1);

  if (!OpenClassesRoot(&hClassesRoot, &closeClassesRoot))
    return FALSE;

  if (fRet = RegCreateKeyExA(hClassesRoot, achIMEKey, 0, NULL,
                             REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey,
                             &dw) == ERROR_SUCCESS) {
    fRet &= RegSetValueExA(hKey, NULL, 0, REG_SZ, (BYTE*)TEXTSERVICE_DESC_A,
                           sizeof TEXTSERVICE_DESC_A) == ERROR_SUCCESS;
    if (fRet &=
        RegCreateKeyExA(hKey, c_szInProcSvr32, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, NULL, &hSubKey, &dw) == ERROR_SUCCESS) {
      dw = GetModuleFileNameA(g_hInst, achFileName, ARRAYSIZE(achFileName));

#ifdef _M_ARM64
      {
        // On ARM64, system-wide installation registers the ARM64X redirector
        // copied as weasel.dll. Per-user installation keeps the packaged
        // filename because the x86 weasel.dll lives in the same directory.
        if (IsPerUserRegistration()) {
          char* fileName = strrchr(achFileName, '\\');
          if (fileName)
            strcpy_s(fileName + 1,
                     ARRAYSIZE(achFileName) - (fileName + 1 - achFileName),
                     "weaselARM64X.dll");
        } else {
          char wrapperPath[MAX_PATH];
          StringCbCatA(achFileName, MAX_PATH, "\\..\\weasel.dll");
          GetFullPathNameA(achFileName, MAX_PATH, wrapperPath, NULL);
          memcpy(achFileName, wrapperPath, MAX_PATH);
        }
      }
#endif

      fRet &= RegSetValueExA(hSubKey, NULL, 0, REG_SZ, (BYTE*)achFileName,
                             (strlen(achFileName) + 1) * sizeof(char)) ==
              ERROR_SUCCESS;
      fRet &= RegSetValueExA(hSubKey, c_szModelName, 0, REG_SZ,
                             (BYTE*)TEXTSERVICE_MODEL,
                             sizeof TEXTSERVICE_MODEL) == ERROR_SUCCESS;
      RegCloseKey(hSubKey);
    }
    RegCloseKey(hKey);
  }
  if (closeClassesRoot)
    RegCloseKey(hClassesRoot);
  return fRet;
}

void UnregisterServer() {
  HKEY hClassesRoot;
  bool closeClassesRoot;
  char achIMEKey[ARRAYSIZE(c_szInfoKeyPrefix) + CLSID_STRLEN];
  if (!CLSIDToStringA(c_clsidTextService,
                      achIMEKey + ARRAYSIZE(c_szInfoKeyPrefix) - 1))
    return;
  memcpy(achIMEKey, c_szInfoKeyPrefix, sizeof(c_szInfoKeyPrefix) - 1);
  if (!OpenClassesRoot(&hClassesRoot, &closeClassesRoot))
    return;
  RecurseDeleteKeyA(hClassesRoot, achIMEKey);

  if (closeClassesRoot) {
    RegCloseKey(hClassesRoot);
    return;
  }

  // On Windows 8, we need to manually delete the registry key for our TIP
  char tipKey[ARRAYSIZE(c_szTipKeyPrefix) + CLSID_STRLEN];
  if (!CLSIDToStringA(c_clsidTextService,
                      tipKey + ARRAYSIZE(c_szTipKeyPrefix) - 1))
    return;
  memcpy(tipKey, c_szTipKeyPrefix, sizeof(c_szTipKeyPrefix) - 1);
  RecurseDeleteKeyA(HKEY_CLASSES_ROOT, tipKey);
}
