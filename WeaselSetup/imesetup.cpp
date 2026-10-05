#include "stdafx.h"
#include <string>
#include <vector>
#include <cstdarg>
#include <msctf.h>
#include <strsafe.h>
#include <StringAlgorithm.hpp>
#include <WeaselConstants.h>
#include <WeaselUtility.h>
#include "InstallOptionsDlg.h"

// {A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}
static const GUID c_clsidTextService = {
    0xa3f4cded,
    0xb1e9,
    0x41ee,
    {0x9c, 0xa6, 0x7b, 0x4d, 0xd, 0xe6, 0xcb, 0xa}};

// {3D02CAB6-2B8E-4781-BA20-1C9267529467}
static const GUID c_guidProfile = {
    0x3d02cab6,
    0x2b8e,
    0x4781,
    {0xba, 0x20, 0x1c, 0x92, 0x67, 0x52, 0x94, 0x67}};

#define ILOT_UNINSTALL 0x00000001
typedef HRESULT(WINAPI* PTF_INSTALLLAYOUTORTIP)(LPCWSTR psz, DWORD dwFlags);
typedef BOOL(WINAPI* PTF_INSTALLLAYOUTORTIPUSERREG)(LPCWSTR pszUserReg,
                                                    LPCWSTR pszSystemReg,
                                                    LPCWSTR pszSoftwareReg,
                                                    LPCWSTR psz,
                                                    DWORD dwFlags);

static void TraceRegistration(const wchar_t* format, ...) {
  WCHAR path[MAX_PATH] = {};
  if (!GetTempPathW(_countof(path), path))
    return;
  if (FAILED(StringCchCatW(path, _countof(path), L"weasel-register.log")))
    return;
  WCHAR message[1024] = {};
  va_list args;
  va_start(args, format);
  StringCchVPrintfW(message, _countof(message), format, args);
  va_end(args);
  HANDLE file =
      CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE)
    return;
  char utf8[4096] = {};
  int length = WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8,
                                   _countof(utf8), NULL, NULL);
  if (length > 1) {
    DWORD written;
    WriteFile(file, utf8, length - 1, &written, NULL);
    static const char newline[] = "\r\n";
    WriteFile(file, newline, sizeof(newline) - 1, &written, NULL);
  }
  CloseHandle(file);
}

#define WEASEL_WER_KEY                            \
  L"SOFTWARE\\Microsoft\\Windows\\Windows Error " \
  L"Reporting\\LocalDumps\\WeaselServer.exe"

BOOL copy_file(const std::wstring& src, const std::wstring& dest) {
  BOOL ret = CopyFile(src.c_str(), dest.c_str(), FALSE);
  if (!ret) {
    for (int i = 0; i < 10; ++i) {
      std::wstring old = dest + L".old." + std::to_wstring(i);
      if (MoveFileEx(dest.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileEx(old.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        break;
      }
    }
    ret = CopyFile(src.c_str(), dest.c_str(), FALSE);
  }
  return ret;
}

BOOL delete_file(const std::wstring& file) {
  BOOL ret = DeleteFile(file.c_str());
  if (!ret) {
    for (int i = 0; i < 10; ++i) {
      std::wstring old = file + L".old." + std::to_wstring(i);
      if (MoveFileEx(file.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileEx(old.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        return TRUE;
      }
    }
  }
  return ret;
}

typedef BOOL(WINAPI* PISWOW64P2)(HANDLE, USHORT*, USHORT*);
BOOL is_arm64_machine() {
  PISWOW64P2 fnIsWow64Process2 = (PISWOW64P2)GetProcAddress(
      GetModuleHandle(_T("kernel32.dll")), "IsWow64Process2");

  if (fnIsWow64Process2 == NULL) {
    return FALSE;
  }

  USHORT processMachine;
  USHORT nativeMachine;

  if (!fnIsWow64Process2(GetCurrentProcess(), &processMachine,
                         &nativeMachine)) {
    return FALSE;
  }
  return nativeMachine == IMAGE_FILE_MACHINE_ARM64;
}

typedef HRESULT(WINAPI* PISWOWGMS)(USHORT, BOOL*);
typedef UINT(WINAPI* PGSW64DIR2)(LPWSTR, UINT, WORD);
INT get_wow_arm32_system_dir(LPWSTR lpBuffer, UINT uSize) {
  PISWOWGMS fnIsWow64GuestMachineSupported = (PISWOWGMS)GetProcAddress(
      GetModuleHandle(_T("kernel32.dll")), "IsWow64GuestMachineSupported");
  PGSW64DIR2 fnGetSystemWow64Directory2W = (PGSW64DIR2)GetProcAddress(
      GetModuleHandle(_T("kernelbase.dll")), "GetSystemWow64Directory2W");

  if (fnIsWow64GuestMachineSupported == NULL ||
      fnGetSystemWow64Directory2W == NULL) {
    return 0;
  }

  BOOL supported;
  if (fnIsWow64GuestMachineSupported(IMAGE_FILE_MACHINE_ARMNT, &supported) !=
      S_OK) {
    return 0;
  }

  if (!supported) {
    return 0;
  }

  return fnGetSystemWow64Directory2W(lpBuffer, uSize, IMAGE_FILE_MACHINE_ARMNT);
}

typedef int (*ime_register_func)(const std::wstring& ime_path,
                                 bool register_ime,
                                 bool is_wow64,
                                 bool is_wowarm,
                                 const std::wstring& profile,
                                 bool silent);

static bool is_per_user_registration() {
  WCHAR value[2];
  return GetEnvironmentVariableW(L"WEASEL_PER_USER", value, _countof(value)) >
         0;
}

static bool override_machine_registry_for_current_user(bool enable) {
  if (!enable)
    return RegOverridePredefKey(HKEY_LOCAL_MACHINE, NULL) == ERROR_SUCCESS;

  HKEY current_user = NULL;
  LSTATUS result = RegOpenCurrentUser(KEY_READ | KEY_WRITE, &current_user);
  if (result != ERROR_SUCCESS)
    return false;

  result = RegOverridePredefKey(HKEY_LOCAL_MACHINE, current_user);
  RegCloseKey(current_user);
  return result == ERROR_SUCCESS;
}

static LANGID profile_to_lang_id(const std::wstring& profile) {
  if (profile == L"hant")
    return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);
  if (profile == L"hongkong")
    return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_HONGKONG);
  if (profile == L"macau")
    return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_MACAU);
  if (profile == L"singapore")
    return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SINGAPORE);
  return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
}

static std::wstring profile_to_title(const std::wstring& profile) {
  WCHAR clsidTextService[64] = {0};
  WCHAR profileGuid[64] = {0};
  WCHAR langidText[5] = {0};

  if (StringFromGUID2(c_clsidTextService, clsidTextService,
                      _countof(clsidTextService)) <= 0 ||
      StringFromGUID2(c_guidProfile, profileGuid, _countof(profileGuid)) <= 0 ||
      FAILED(StringCchPrintfW(langidText, _countof(langidText), L"%04X",
                              profile_to_lang_id(profile)))) {
    return L"";
  }

  return std::wstring(langidText) + L":" + clsidTextService + profileGuid;
}

static bool read_reg_string(HKEY key, const WCHAR* name, std::wstring& value) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, NULL, &type, NULL, &bytes) != ERROR_SUCCESS ||
      type != REG_SZ || bytes < sizeof(WCHAR))
    return false;

  std::vector<WCHAR> buffer(bytes / sizeof(WCHAR) + 1, L'\0');
  if (RegQueryValueExW(key, name, NULL, &type,
                       reinterpret_cast<LPBYTE>(buffer.data()),
                       &bytes) != ERROR_SUCCESS)
    return false;

  value.assign(buffer.data());
  return true;
}

static bool write_reg_string(HKEY key,
                             const WCHAR* name,
                             const std::wstring& value) {
  return RegSetValueExW(
             key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
             static_cast<DWORD>((value.size() + 1) * sizeof(WCHAR))) ==
         ERROR_SUCCESS;
}

static std::wstring profile_to_language_tag(const std::wstring& profile) {
  if (profile == L"hant")
    return L"zh-Hant-TW";
  if (profile == L"hongkong")
    return L"zh-Hant-HK";
  if (profile == L"macau")
    return L"zh-Hant-MO";
  if (profile == L"singapore")
    return L"zh-Hans-SG";
  return L"zh-Hans-CN";
}

static bool update_user_profile_input_method(const std::wstring& profile,
                                             bool add) {
  const std::wstring user_profile =
      L"Control Panel\\International\\User Profile\\" +
      profile_to_language_tag(profile);
  const std::wstring tip = profile_to_title(profile);

  HKEY key = NULL;
  DWORD disposition = 0;
  LSTATUS status =
      add ? RegCreateKeyExW(HKEY_CURRENT_USER, user_profile.c_str(), 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &key,
                            &disposition)
          : RegOpenKeyExW(HKEY_CURRENT_USER, user_profile.c_str(), 0,
                          KEY_SET_VALUE, &key);

  if (status == ERROR_FILE_NOT_FOUND && !add)
    return true;
  if (status != ERROR_SUCCESS) {
    TraceRegistration(L"User Profile open status=%ld path=%s", status,
                      user_profile.c_str());
    return false;
  }

  bool ok = false;
  if (add) {
    ok = write_reg_string(key, tip.c_str(), L"2");
    TraceRegistration(L"User Profile add path=%s value=%s success=%d",
                      user_profile.c_str(), tip.c_str(), ok);
  } else {
    status = RegDeleteValueW(key, tip.c_str());
    ok = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    TraceRegistration(L"User Profile remove status=%ld path=%s value=%s",
                      status, user_profile.c_str(), tip.c_str());
  }

  RegCloseKey(key);
  return ok;
}

static bool update_user_input_method(const std::wstring& profile, bool add) {
  WCHAR clsid[64] = {};
  WCHAR profile_guid[64] = {};
  WCHAR category_guid[64] = {};
  WCHAR lang_id[16] = {};

  if (StringFromGUID2(c_clsidTextService, clsid, _countof(clsid)) <= 0 ||
      StringFromGUID2(c_guidProfile, profile_guid, _countof(profile_guid)) <=
          0 ||
      StringFromGUID2(GUID_TFCAT_TIP_KEYBOARD, category_guid,
                      _countof(category_guid)) <= 0 ||
      FAILED(StringCchPrintfW(lang_id, _countof(lang_id), L"0x%08X",
                              profile_to_lang_id(profile)))) {
    return false;
  }

  const std::wstring base =
      L"Software\\Microsoft\\CTF\\SortOrder\\AssemblyItem\\" +
      std::wstring(lang_id) + L"\\" + category_guid;

  HKEY base_key = NULL;
  DWORD disposition = 0;
  LSTATUS status = RegCreateKeyExW(
      HKEY_CURRENT_USER, base.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE,
      KEY_READ | KEY_WRITE, NULL, &base_key, &disposition);
  TraceRegistration(L"SortOrder base status=%ld path=%s", status, base.c_str());
  if (status != ERROR_SUCCESS)
    return false;

  std::vector<std::wstring> matching_slots;
  DWORD first_free = 0xFFFFFFFF;
  for (DWORD slot = 0; slot < 256; ++slot) {
    WCHAR slot_name[16] = {};
    StringCchPrintfW(slot_name, _countof(slot_name), L"%08X", slot);

    HKEY slot_key = NULL;
    status = RegOpenKeyExW(base_key, slot_name, 0, KEY_READ, &slot_key);
    if (status == ERROR_FILE_NOT_FOUND) {
      if (first_free == 0xFFFFFFFF)
        first_free = slot;
      continue;
    }
    if (status != ERROR_SUCCESS)
      continue;

    std::wstring existing_clsid;
    std::wstring existing_profile;
    const bool matches =
        read_reg_string(slot_key, L"CLSID", existing_clsid) &&
        read_reg_string(slot_key, L"Profile", existing_profile) &&
        _wcsicmp(existing_clsid.c_str(), clsid) == 0 &&
        _wcsicmp(existing_profile.c_str(), profile_guid) == 0;
    RegCloseKey(slot_key);

    if (matches)
      matching_slots.emplace_back(slot_name);
  }

  if (!add) {
    bool ok = true;
    for (const auto& slot : matching_slots) {
      status = RegDeleteTreeW(base_key, slot.c_str());
      TraceRegistration(L"SortOrder remove slot=%s status=%ld", slot.c_str(),
                        status);
      if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        ok = false;
    }
    RegCloseKey(base_key);
    return update_user_profile_input_method(profile, false) && ok;
  }

  if (!matching_slots.empty()) {
    TraceRegistration(L"SortOrder already present slot=%s",
                      matching_slots.front().c_str());
    RegCloseKey(base_key);
    return update_user_profile_input_method(profile, true);
  }

  if (first_free == 0xFFFFFFFF) {
    RegCloseKey(base_key);
    return false;
  }

  WCHAR slot_name[16] = {};
  StringCchPrintfW(slot_name, _countof(slot_name), L"%08X", first_free);
  HKEY slot_key = NULL;
  status =
      RegCreateKeyExW(base_key, slot_name, 0, NULL, REG_OPTION_NON_VOLATILE,
                      KEY_WRITE, NULL, &slot_key, &disposition);
  if (status != ERROR_SUCCESS) {
    RegCloseKey(base_key);
    return false;
  }

  const bool ok = write_reg_string(slot_key, L"CLSID", clsid) &&
                  write_reg_string(slot_key, L"Profile", profile_guid);
  const DWORD keyboard_layout = 0;
  const bool layout_ok =
      RegSetValueExW(slot_key, L"KeyboardLayout", 0, REG_DWORD,
                     reinterpret_cast<const BYTE*>(&keyboard_layout),
                     sizeof(keyboard_layout)) == ERROR_SUCCESS;
  TraceRegistration(L"SortOrder add slot=%s success=%d", slot_name,
                    ok && layout_ok);
  RegCloseKey(slot_key);
  RegCloseKey(base_key);

  return ok && layout_ok && update_user_profile_input_method(profile, true);
}

int install_ime_file(std::wstring& srcPath,
                     const std::wstring& ext,
                     const std::wstring& profile,
                     bool silent,
                     bool per_user,
                     ime_register_func func) {
  WCHAR path[MAX_PATH];
  GetModuleFileNameW(GetModuleHandle(NULL), path, _countof(path));

  std::wstring srcFileName = L"weasel";

  srcFileName += ext;
  WCHAR drive[_MAX_DRIVE];
  WCHAR dir[_MAX_DIR];
  _wsplitpath_s(path, drive, _countof(drive), dir, _countof(dir), NULL, 0, NULL,
                0);
  srcPath = std::wstring(drive) + dir + srcFileName;

  if (per_user) {
    SetEnvironmentVariableW(L"WEASEL_PER_USER", L"1");
    int retval = func(srcPath, true, false, false, profile, silent);
    if (is_wow64()) {
      PVOID oldValue = NULL;
      if (Wow64DisableWow64FsRedirection(&oldValue) == FALSE) {
        MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERRCANCELFSREDIRECT,
                              IDS_STR_INSTALL_FAILED, MB_ICONERROR | MB_OK);
        return 1;
      }

      if (is_arm64_machine()) {
        WCHAR sysarm32[MAX_PATH];
        if (get_wow_arm32_system_dir(sysarm32, _countof(sysarm32)) > 0) {
          std::wstring arm32Path = srcPath;
          ireplace_last(arm32Path, ext, L"ARM" + ext);
          retval += func(arm32Path, true, true, true, profile, silent);
        }

        std::wstring arm64Path = srcPath;
        ireplace_last(arm64Path, ext, L"ARM64" + ext);
        retval += func(arm64Path, true, true, false, profile, silent);
      } else {
        std::wstring x64Path = srcPath;
        ireplace_last(x64Path, ext, L"x64" + ext);
        retval += func(x64Path, true, true, false, profile, silent);
      }

      if (Wow64RevertWow64FsRedirection(oldValue) == FALSE) {
        MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERRRECOVERFSREDIRECT,
                              IDS_STR_INSTALL_FAILED, MB_ICONERROR | MB_OK);
        return 1;
      }
    }
    return retval;
  }

  GetSystemDirectoryW(path, _countof(path));
  std::wstring destPath = std::wstring(path) + L"\\weasel" + ext;

  int retval = 0;
  // 复制 .dll/.ime 到系统目录
  if (!copy_file(srcPath, destPath)) {
    MSG_NOT_SILENT_ID_CAP(silent, destPath.c_str(), IDS_STR_INSTALL_FAILED,
                          MB_ICONERROR | MB_OK);
    return 1;
  }
  retval += func(destPath, true, false, false, profile, silent);
  if (is_wow64()) {
    PVOID OldValue = NULL;
    // PW64DW64FR fnWow64DisableWow64FsRedirection =
    // (PW64DW64FR)GetProcAddress(GetModuleHandle(_T("kernel32.dll")),
    // "Wow64DisableWow64FsRedirection"); PW64RW64FR
    // fnWow64RevertWow64FsRedirection =
    // (PW64RW64FR)GetProcAddress(GetModuleHandle(_T("kernel32.dll")),
    // "Wow64RevertWow64FsRedirection");
    if (Wow64DisableWow64FsRedirection(&OldValue) == FALSE) {
      MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERRCANCELFSREDIRECT,
                            IDS_STR_INSTALL_FAILED, MB_ICONERROR | MB_OK);
      return 1;
    }

    if (is_arm64_machine()) {
      WCHAR sysarm32[MAX_PATH];
      if (get_wow_arm32_system_dir(sysarm32, _countof(sysarm32)) > 0) {
        // Install the ARM32 version if ARM32 WOW is supported （lower than
        // Windows 11 24H2).
        std::wstring srcPathARM32 = srcPath;
        ireplace_last(srcPathARM32, ext, L"ARM" + ext);

        std::wstring destPathARM32 = std::wstring(sysarm32) + L"\\weasel" + ext;
        if (!copy_file(srcPathARM32, destPathARM32)) {
          MSG_NOT_SILENT_ID_CAP(silent, destPathARM32.c_str(),
                                IDS_STR_INSTALL_FAILED, MB_ICONERROR | MB_OK);
          return 1;
        }
        retval += func(destPathARM32, true, true, true, profile, silent);
      }

      // Then install the ARM64 (and x64) version.
      // On ARM64 weasel.dll(ime) is an ARM64X redirection DLL (weaselARM64X).
      // When loaded, it will be redirected to weaselARM64.dll(ime) on ARM64
      // processes, and weaselx64.dll(ime) on x64 processes. So we need a total
      // of three files.

      std::wstring srcPathX64 = srcPath;
      std::wstring destPathX64 = destPath;
      ireplace_last(srcPathX64, ext, L"x64" + ext);
      ireplace_last(destPathX64, ext, L"x64" + ext);
      if (!copy_file(srcPathX64, destPathX64)) {
        MSG_NOT_SILENT_ID_CAP(silent, destPathX64.c_str(),
                              IDS_STR_INSTALL_FAILED, MB_ICONERROR | MB_OK);
        return 1;
      }

      std::wstring srcPathARM64 = srcPath;
      std::wstring destPathARM64 = destPath;
      ireplace_last(srcPathARM64, ext, L"ARM64" + ext);
      ireplace_last(destPathARM64, ext, L"ARM64" + ext);
      if (!copy_file(srcPathARM64, destPathARM64)) {
        MSG_NOT_SILENT_ID_CAP(silent, destPathARM64.c_str(),
                              IDS_STR_INSTALL_FAILED, MB_ICONERROR | MB_OK);
        return 1;
      }

      // Since weaselARM64X is just a redirector we don't have separate
      // profile variants.
      srcPath = std::wstring(drive) + dir + L"weaselARM64X" + ext;
    } else {
      ireplace_last(srcPath, ext, L"x64" + ext);
    }

    if (!copy_file(srcPath, destPath)) {
      MSG_NOT_SILENT_ID_CAP(silent, destPath.c_str(), IDS_STR_INSTALL_FAILED,
                            MB_ICONERROR | MB_OK);
      return 1;
    }
    retval += func(destPath, true, true, false, profile, silent);
    if (Wow64RevertWow64FsRedirection(OldValue) == FALSE) {
      MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERRRECOVERFSREDIRECT,
                            IDS_STR_INSTALL_FAILED, MB_ICONERROR | MB_OK);
      return 1;
    }
  }
  return retval;
}

int uninstall_ime_file(const std::wstring& ext,
                       const std::wstring& profile,
                       bool silent,
                       bool per_user,
                       ime_register_func func) {
  int retval = 0;
  if (per_user) {
    SetEnvironmentVariableW(L"WEASEL_PER_USER", L"1");
    WCHAR modulePath[MAX_PATH];
    GetModuleFileNameW(GetModuleHandle(NULL), modulePath, _countof(modulePath));
    std::wstring basePath(modulePath);
    basePath.resize(basePath.find_last_of(L"\\") + 1);

    std::wstring imePath = basePath + L"weasel" + ext;
    retval += func(imePath, false, false, false, profile, silent);
    if (is_wow64()) {
      PVOID oldValue = NULL;
      if (Wow64DisableWow64FsRedirection(&oldValue) == FALSE) {
        MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERRCANCELFSREDIRECT,
                              IDS_STR_UNINSTALL_FAILED, MB_ICONERROR | MB_OK);
        return 1;
      }

      if (is_arm64_machine()) {
        WCHAR sysarm32[MAX_PATH];
        if (get_wow_arm32_system_dir(sysarm32, _countof(sysarm32)) > 0) {
          std::wstring arm32Path = basePath + L"weaselARM" + ext;
          retval += func(arm32Path, false, true, true, profile, silent);
        }
        std::wstring arm64Path = basePath + L"weaselARM64" + ext;
        retval += func(arm64Path, false, true, false, profile, silent);
      } else {
        std::wstring x64Path = basePath + L"weaselx64" + ext;
        retval += func(x64Path, false, true, false, profile, silent);
      }

      if (Wow64RevertWow64FsRedirection(oldValue) == FALSE) {
        MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERRRECOVERFSREDIRECT,
                              IDS_STR_UNINSTALL_FAILED, MB_ICONERROR | MB_OK);
        return 1;
      }
    }
    return retval;
  }
  WCHAR path[MAX_PATH];
  GetSystemDirectoryW(path, _countof(path));
  std::wstring imePath(path);
  imePath += L"\\weasel" + ext;
  retval += func(imePath, false, false, false, profile, silent);
  delete_file(imePath);
  if (is_wow64()) {
    retval += func(imePath, false, true, false, profile, silent);
    PVOID OldValue = NULL;
    if (Wow64DisableWow64FsRedirection(&OldValue) == FALSE) {
      MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERRCANCELFSREDIRECT,
                            IDS_STR_UNINSTALL_FAILED, MB_ICONERROR | MB_OK);
      return 1;
    }

    if (is_arm64_machine()) {
      WCHAR sysarm32[MAX_PATH];
      if (get_wow_arm32_system_dir(sysarm32, _countof(sysarm32)) > 0) {
        std::wstring imePathARM32 = std::wstring(sysarm32) + L"\\weasel" + ext;
        retval += func(imePathARM32, false, true, true, profile, silent);
        delete_file(imePathARM32);
      }

      std::wstring imePathX64 = imePath;
      ireplace_last(imePathX64, ext, L"x64" + ext);
      delete_file(imePathX64);

      std::wstring imePathARM64 = imePath;
      ireplace_last(imePathARM64, ext, L"ARM64" + ext);
      delete_file(imePathARM64);
    }

    delete_file(imePath);
    if (Wow64RevertWow64FsRedirection(OldValue) == FALSE) {
      MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERRRECOVERFSREDIRECT,
                            IDS_STR_UNINSTALL_FAILED, MB_ICONERROR | MB_OK);
      return 1;
    }
  }
  return retval;
}

// 注册IME输入法
// `register_ime` (IMM/.ime) support removed — TSF-only build

void enable_profile(BOOL fEnable,
                    const std::wstring& profile,
                    bool per_user = false) {
  if (per_user && !override_machine_registry_for_current_user(true)) {
    return;
  }

  HRESULT hr;
  ITfInputProcessorProfiles* pProfiles = NULL;

  hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL,
                        CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                        (LPVOID*)&pProfiles);

  if (SUCCEEDED(hr)) {
    LANGID lang_id = profile_to_lang_id(profile);
    if (fEnable) {
      pProfiles->EnableLanguageProfile(c_clsidTextService, lang_id,
                                       c_guidProfile, fEnable);
      pProfiles->EnableLanguageProfileByDefault(c_clsidTextService, lang_id,
                                                c_guidProfile, fEnable);
    } else {
      pProfiles->RemoveLanguageProfile(c_clsidTextService, lang_id,
                                       c_guidProfile);
    }

    pProfiles->Release();
  }

  if (per_user)
    override_machine_registry_for_current_user(false);
}

// 注册TSF输入法
int register_text_service(const std::wstring& tsf_path,
                          bool register_ime,
                          bool is_wow64,
                          bool is_wowarm32,
                          const std::wstring& profile,
                          bool silent) {
  using RegisterServerFunction = HRESULT(STDAPICALLTYPE*)();
  const bool per_user = is_per_user_registration();

  if (!register_ime && !per_user)
    enable_profile(FALSE, profile, per_user);

  std::wstring params;
  if (per_user) {
    params = L" /s /n ";
    if (!register_ime)
      params += L"/u ";
    params += L"/i:user," + profile + L" \"" + tsf_path + L"\"";
  } else {
    params = L" \"" + tsf_path + L"\"";
    if (!register_ime)
      params = L" /u " + params;  // unregister
    params = L" /s " + params;

    if (!SetEnvironmentVariable(L"TEXTSERVICE_PROFILE", profile.c_str())) {
      throw std::runtime_error("SetEnvironmentVariable failed");
    }
  }

  TraceRegistration(L"WeaselSetup regsvr32 path=%s params=%s", tsf_path.c_str(),
                    params.c_str());

  std::wstring app = L"regsvr32.exe";
  if (is_wowarm32) {
    WCHAR sysarm32[MAX_PATH];
    get_wow_arm32_system_dir(sysarm32, _countof(sysarm32));

    app = std::wstring(sysarm32) + L"\\" + app;
  }

  SHELLEXECUTEINFOW shExInfo = {0};
  shExInfo.cbSize = sizeof(shExInfo);
  shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
  shExInfo.hwnd = 0;
  shExInfo.lpVerb = L"open";               // Operation to perform
  shExInfo.lpFile = app.c_str();           // Application to start
  shExInfo.lpParameters = params.c_str();  // Additional parameters
  shExInfo.lpDirectory = 0;
  shExInfo.nShow = SW_SHOW;
  shExInfo.hInstApp = 0;
  if (ShellExecuteExW(&shExInfo)) {
    WaitForSingleObject(shExInfo.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(shExInfo.hProcess, &exit_code);
    CloseHandle(shExInfo.hProcess);
    TraceRegistration(L"regsvr32 exit=%lu", exit_code);
    if (exit_code != 0)
      return 1;
  } else {
    WCHAR msg[100];
    CString str;
    str.LoadStringW(IDS_STR_ERRREGTSF);
    StringCchPrintfW(msg, _countof(msg), str, params.c_str());
    MSG_NOT_SILENT_ID_CAP(silent, msg, IDS_STR_INORUN_FAILED,
                          MB_ICONERROR | MB_OK);
    return 1;
  }

  if (register_ime && !per_user)
    enable_profile(TRUE, profile, per_user);

  return 0;
}

int install(const std::wstring& profile, bool silent, bool per_user) {
  std::wstring ime_src_path;
  int retval = 0;

  if (per_user) {
    WCHAR trace_path[MAX_PATH] = {};
    if (GetTempPathW(_countof(trace_path), trace_path) &&
        SUCCEEDED(StringCchCatW(trace_path, _countof(trace_path),
                                L"weasel-register.log")))
      DeleteFileW(trace_path);
  }

  retval += install_ime_file(ime_src_path, L".dll", profile, silent, per_user,
                             &register_text_service);

  // 写注册表
  WCHAR drive[_MAX_DRIVE];
  WCHAR dir[_MAX_DIR];
  _wsplitpath_s(ime_src_path.c_str(), drive, _countof(drive), dir,
                _countof(dir), NULL, 0, NULL, 0);
  std::wstring rootDir = std::wstring(drive) + dir;
  rootDir.pop_back();
  HKEY install_root = per_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
  auto ret = SetRegKeyValue(install_root, WEASEL_REG_KEY, L"WeaselRoot",
                            rootDir.c_str(), REG_SZ);
  if (FAILED(HRESULT_FROM_WIN32(ret))) {
    MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERRWRITEWEASELROOT,
                          IDS_STR_INSTALL_FAILED, MB_ICONERROR | MB_OK);
    return 1;
  }

  const std::wstring executable = L"WeaselServer.exe";
  ret = SetRegKeyValue(install_root, WEASEL_REG_KEY, L"ServerExecutable",
                       executable.c_str(), REG_SZ);
  if (FAILED(HRESULT_FROM_WIN32(ret))) {
    MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERRREGIMEWRITESVREXE,
                          IDS_STR_INSTALL_FAILED, MB_ICONERROR | MB_OK);
    return 1;
  }

  // persist the installing profile so that uninstall removes the right one
  const WCHAR PROFILE_KEY[] = L"Software\\Rime\\Weasel";
  ret = SetRegKeyValue(HKEY_CURRENT_USER, PROFILE_KEY, L"Profile",
                       profile.c_str(), REG_SZ);
  if (FAILED(HRESULT_FROM_WIN32(ret))) {
    MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERR_WRITE_PROFILE,
                          IDS_STR_INSTALL_FAILED, MB_ICONERROR | MB_OK);
    return 1;
  }
  ret = SetRegKeyValue(HKEY_CURRENT_USER, PROFILE_KEY, L"Hant",
                       (profile == L"hant" ? 1 : 0), REG_DWORD);
  if (FAILED(HRESULT_FROM_WIN32(ret))) {
    MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_ERR_WRITE_HANT,
                          IDS_STR_INSTALL_FAILED, MB_ICONERROR | MB_OK);
    return 1;
  }

  // Enable the installed profile for the current user.
  HMODULE hInputDLL = LoadLibrary(TEXT("input.dll"));
  if (hInputDLL) {
    std::wstring title = profile_to_title(profile);
    if (!title.empty()) {
      if (!per_user) {
        auto pfnInstallLayoutOrTip = (PTF_INSTALLLAYOUTORTIP)GetProcAddress(
            hInputDLL, "InstallLayoutOrTip");
        if (pfnInstallLayoutOrTip)
          (*pfnInstallLayoutOrTip)(title.c_str(), 0);
      }
    }
    FreeLibrary(hInputDLL);
  }

  // https://learn.microsoft.com/zh-cn/windows/win32/wer/collecting-user-mode-dumps
  if (!per_user) {
    const std::wstring dmpPathW = WeaselLogPath().wstring();
    SetRegKeyValue(HKEY_LOCAL_MACHINE, WEASEL_WER_KEY, L"DumpFolder",
                   dmpPathW.c_str(), REG_SZ, true);
    SetRegKeyValue(HKEY_LOCAL_MACHINE, WEASEL_WER_KEY, L"DumpType", 0,
                   REG_DWORD, true);
    SetRegKeyValue(HKEY_LOCAL_MACHINE, WEASEL_WER_KEY, L"CustomDumpFlags", 0,
                   REG_DWORD, true);
    SetRegKeyValue(HKEY_LOCAL_MACHINE, WEASEL_WER_KEY, L"DumpCount", 10,
                   REG_DWORD, true);
  }

  if (retval)
    return 1;

  if (per_user && !update_user_input_method(profile, true))
    return 1;

  MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_INSTALL_SUCCESS_INFO,
                        IDS_STR_INSTALL_SUCCESS_CAP,
                        MB_ICONINFORMATION | MB_OK);
  return 0;
}

int uninstall(bool silent, bool per_user) {
  // 注销输入法
  int retval = 0;

  const WCHAR KEY[] = L"Software\\Rime\\Weasel";
  HKEY hKey;
  std::wstring profile = L"hans";
  LSTATUS ret = RegOpenKey(HKEY_CURRENT_USER, KEY, &hKey);
  if (ret == ERROR_SUCCESS) {
    DWORD type = 0;
    DWORD data = 0;
    WCHAR value[MAX_PATH] = {0};
    DWORD len = sizeof(value);
    ret = RegQueryValueEx(hKey, L"Profile", NULL, &type, (LPBYTE)value, &len);
    if (ret == ERROR_SUCCESS && type == REG_SZ && value[0] != L'\0') {
      profile = value;
    } else {
      len = sizeof(data);
      ret = RegQueryValueEx(hKey, L"Hant", NULL, &type, (LPBYTE)&data, &len);
      if (ret == ERROR_SUCCESS && type == REG_DWORD) {
        profile = (data != 0) ? L"hant" : L"hans";
      }
    }

    if (per_user) {
      update_user_input_method(profile, false);
    } else {
      HMODULE hInputDLL = LoadLibrary(TEXT("input.dll"));
      if (hInputDLL) {
        std::wstring title = profile_to_title(profile);
        if (!title.empty()) {
          auto pfnInstallLayoutOrTip = (PTF_INSTALLLAYOUTORTIP)GetProcAddress(
              hInputDLL, "InstallLayoutOrTip");
          if (pfnInstallLayoutOrTip)
            (*pfnInstallLayoutOrTip)(title.c_str(), ILOT_UNINSTALL);
        }
        FreeLibrary(hInputDLL);
      }
    }
    RegCloseKey(hKey);
  }

  // IMM/.ime support removed; only uninstall TSF/.dll
  retval += uninstall_ime_file(L".dll", profile, silent, per_user,
                               &register_text_service);

  // 清除注册信息
  HKEY install_root = per_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
  RegDeleteKey(install_root, WEASEL_REG_KEY);
  if (!per_user)
    RegDeleteKey(install_root, RIME_REG_KEY);

  // delete WER register,
  // "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\Windows Error
  // Reporting\\LocalDumps\\WeaselServer.exe" no WOW64 redirect

  if (!per_user) {
    auto flag_wow64 = is_wow64() ? KEY_WOW64_64KEY : 0;
    RegDeleteKeyEx(HKEY_LOCAL_MACHINE, WEASEL_WER_KEY, flag_wow64, 0);
  }
  if (retval)
    return 1;

  MSG_NOT_SILENT_BY_IDS(silent, IDS_STR_UNINSTALL_SUCCESS_INFO,
                        IDS_STR_UNINSTALL_SUCCESS_CAP,
                        MB_ICONINFORMATION | MB_OK);
  return 0;
}

bool has_installed(bool per_user) {
  if (per_user) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, WEASEL_REG_KEY, 0, KEY_READ, &hKey) !=
        ERROR_SUCCESS)
      return false;
    WCHAR root[MAX_PATH];
    DWORD size = sizeof(root);
    DWORD type = 0;
    LSTATUS ret =
        RegQueryValueExW(hKey, L"WeaselRoot", NULL, &type, (LPBYTE)root, &size);
    RegCloseKey(hKey);
    return ret == ERROR_SUCCESS && type == REG_SZ && root[0] != L'\0';
  }

  WCHAR path[MAX_PATH];
  GetSystemDirectory(path, _countof(path));
  std::wstring sysPath(path);
  DWORD attr = GetFileAttributesW((sysPath + L"\\weasel.dll").c_str());
  return (attr != INVALID_FILE_ATTRIBUTES &&
          !(attr & FILE_ATTRIBUTE_DIRECTORY));
}
