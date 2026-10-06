#include <windows.h>
#include <msctf.h>
#include <cstdio>
#include <cwchar>

static const GUID kWeaselClsid = {
    0xA3F4CDED, 0xB1E9, 0x41EE,
    {0x9C, 0xA6, 0x7B, 0x4D, 0x0D, 0xE6, 0xCB, 0x0A}};
static const GUID kWeaselProfile = {
    0x3D02CAB6, 0x2B8E, 0x4781,
    {0xBA, 0x20, 0x1C, 0x92, 0x67, 0x52, 0x94, 0x67}};
static const GUID kBopomofoClsid = {
    0xB115690A, 0xEA02, 0x48D5,
    {0xA2, 0x31, 0xE3, 0x57, 0x8D, 0x2F, 0xDF, 0x80}};
static const GUID kBopomofoProfile = {
    0xB2F9C502, 0x1742, 0x11D4,
    {0x97, 0x90, 0x00, 0x80, 0xC8, 0x82, 0x68, 0x7E}};

static void print_guid(const GUID& g) {
  std::printf(
      "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
      g.Data1, g.Data2, g.Data3,
      g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
      g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static HRESULT dump_profiles(ITfInputProcessorProfileMgr* mgr) {
  IEnumTfInputProcessorProfiles* e = nullptr;
  HRESULT hr = mgr->EnumProfiles(0x0404, &e);
  if (FAILED(hr)) {
    std::printf("EnumProfiles failed: 0x%08X\n",
                static_cast<unsigned>(hr));
    return hr;
  }

  unsigned count = 0;
  TF_INPUTPROCESSORPROFILE item = {};
  ULONG fetched = 0;
  while (e->Next(1, &item, &fetched) == S_OK) {
    ++count;
    std::printf("Profile[%u] type=%lu langid=0x%04X clsid=",
                count, item.dwProfileType, item.langid);
    print_guid(item.clsid);
    std::printf(" profile=");
    print_guid(item.guidProfile);
    std::printf(" flags=0x%08lX hkl=0x%p\n",
                item.dwFlags, item.hkl);
  }
  std::printf("Profile count: %u\n", count);

  e->Release();
  return S_OK;
}

static void print_identity() {
  CHAR user[256] = {};
  DWORD user_len = ARRAYSIZE(user);
  if (GetUserNameA(user, &user_len))
    std::printf("User: %s\n", user);

  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    if (GetTokenInformation(token, TokenElevation, &elevation,
                            sizeof(elevation), &size)) {
      std::printf("Token elevated: %lu\n",
                  static_cast<unsigned long>(elevation.TokenIsElevated));
    }
    CloseHandle(token);
  }

  SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
  PSID admin_group = nullptr;
  BOOL is_admin = FALSE;
  if (AllocateAndInitializeSid(
          &nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
          DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admin_group)) {
    CheckTokenMembership(nullptr, admin_group, &is_admin);
    FreeSid(admin_group);
  }
  std::printf("Admin token membership: %s\n", is_admin ? "yes" : "no");
}

static void print_registration_keys() {
  const WCHAR* tip_path =
      L"Software\\Microsoft\\CTF\\TIP\\{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}";
  const WCHAR* profile_path =
      L"Software\\Microsoft\\CTF\\TIP\\{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}\\LanguageProfile\\0x00000404\\{3D02CAB6-2B8E-4781-BA20-1C9267529467}";
  HKEY key = nullptr;
  LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, tip_path, 0,
                                 KEY_READ, &key);
  std::printf("HKCU TIP root status: %ld\n", static_cast<long>(status));
  if (status == ERROR_SUCCESS)
    RegCloseKey(key);

  key = nullptr;
  status = RegOpenKeyExW(HKEY_CURRENT_USER, profile_path, 0, KEY_READ, &key);
  std::printf("HKCU profile key status: %ld\n", static_cast<long>(status));
  if (status == ERROR_SUCCESS)
    RegCloseKey(key);

  key = nullptr;
  status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, tip_path, 0,
                         KEY_READ | KEY_WOW64_64KEY, &key);
  std::printf("HKLM TIP root status: %ld\n", static_cast<long>(status));
  if (status == ERROR_SUCCESS)
    RegCloseKey(key);

  key = nullptr;
  status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, profile_path, 0,
                         KEY_READ | KEY_WOW64_64KEY, &key);
  std::printf("HKLM profile key status: %ld\n", static_cast<long>(status));
  if (status == ERROR_SUCCESS)
    RegCloseKey(key);
}

static void print_user_install_state() {
  CHAR local_app_data[MAX_PATH] = {};
  DWORD env_len = GetEnvironmentVariableA(
      "LOCALAPPDATA", local_app_data, ARRAYSIZE(local_app_data));
  if (env_len > 0 && env_len < ARRAYSIZE(local_app_data))
    std::printf("LOCALAPPDATA: %s\n", local_app_data);

  HKEY key = nullptr;
  LSTATUS status = RegOpenKeyExA(HKEY_CURRENT_USER,
                                 "Software\\Rime\\Weasel", 0,
                                 KEY_READ, &key);
  std::printf("HKCU Software\\Rime\\Weasel status: %ld\n",
              static_cast<long>(status));
  if (status != ERROR_SUCCESS)
    return;

  const char* value_names[] = {"WeaselRoot", "InstallDir"};
  for (const char* name : value_names) {
    CHAR value[MAX_PATH] = {};
    DWORD type = 0;
    DWORD size = sizeof(value);
    LSTATUS query = RegQueryValueExA(
        key, name, nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
    if (query == ERROR_SUCCESS)
      std::printf("HKCU %s: %s\n", name, value);
    else
      std::printf("HKCU %s query status: %ld\n", name,
                  static_cast<long>(query));
  }
  RegCloseKey(key);
}

static void probe_hklm_override_registration(ITfInputProcessorProfileMgr* mgr) {
  std::puts("=== Probe official TSF registration with HKLM overridden to HKCU ===");

  HKEY current_user = nullptr;
  LSTATUS open_status = RegOpenCurrentUser(KEY_READ | KEY_WRITE, &current_user);
  std::printf("RegOpenCurrentUser status: %ld\n", static_cast<long>(open_status));
  if (open_status != ERROR_SUCCESS)
    return;

  LSTATUS override_status =
      RegOverridePredefKey(HKEY_LOCAL_MACHINE, current_user);
  std::printf("RegOverridePredefKey(HKLM -> HKCU) status: %ld\n",
              static_cast<long>(override_status));
  if (override_status != ERROR_SUCCESS) {
    RegCloseKey(current_user);
    return;
  }

  ITfInputProcessorProfiles* profiles = nullptr;
  HRESULT create_profiles = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(&profiles));
  std::printf("Override ITfInputProcessorProfiles CoCreate HRESULT: 0x%08X\n",
              static_cast<unsigned>(create_profiles));
  if (profiles) {
    HRESULT register_service = profiles->Register(kWeaselClsid);
    std::printf("Override Register text service HRESULT: 0x%08X\n",
                static_cast<unsigned>(register_service));
    profiles->Release();
  }

  WCHAR icon_file[MAX_PATH] = {};
  ULONG icon_len =
      GetModuleFileNameW(nullptr, icon_file, ARRAYSIZE(icon_file));
  const WCHAR description[] = L"Weasel";
  HRESULT register_profile = mgr->RegisterProfile(
      kWeaselClsid, 0x0404, kWeaselProfile, description,
      ARRAYSIZE(description) - 1, icon_file, icon_len, 0, nullptr, 0, TRUE, 0);
  std::printf("Override RegisterProfile HRESULT: 0x%08X\n",
              static_cast<unsigned>(register_profile));

  ITfCategoryMgr* category_mgr = nullptr;
  HRESULT create_category = CoCreateInstance(
      CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
      reinterpret_cast<void**>(&category_mgr));
  std::printf("Override ITfCategoryMgr CoCreate HRESULT: 0x%08X\n",
              static_cast<unsigned>(create_category));
  if (category_mgr) {
    HRESULT category_tip = category_mgr->RegisterCategory(
        kWeaselClsid, GUID_TFCAT_CATEGORY_OF_TIP, kWeaselClsid);
    HRESULT category_keyboard = category_mgr->RegisterCategory(
        kWeaselClsid, GUID_TFCAT_TIP_KEYBOARD, kWeaselClsid);
    std::printf("Override RegisterCategory(CATEGORY_OF_TIP) HRESULT: 0x%08X\n",
                static_cast<unsigned>(category_tip));
    std::printf("Override RegisterCategory(TIP_KEYBOARD) HRESULT: 0x%08X\n",
                static_cast<unsigned>(category_keyboard));
    category_mgr->Release();
  }

  LSTATUS restore_status = RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
  std::printf("Restore HKLM override status: %ld\n",
              static_cast<long>(restore_status));
  RegCloseKey(current_user);

  std::puts("=== Registration keys after official APIs under override ===");
  print_registration_keys();
  std::puts("=== ProfileMgr after official APIs under override ===");
  dump_profiles(mgr);
}

static void probe_user_activation(ITfInputProcessorProfileMgr* mgr) {
  ITfInputProcessorProfiles* profiles = nullptr;
  HRESULT create_profiles = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfiles,
      reinterpret_cast<void**>(&profiles));
  std::printf("Activation ITfInputProcessorProfiles CoCreate HRESULT: 0x%08X\n",
              static_cast<unsigned>(create_profiles));
  if (SUCCEEDED(create_profiles)) {
    BOOL enabled = FALSE;
    HRESULT is_enabled = profiles->IsEnabledLanguageProfile(
        kWeaselClsid, 0x0404, kWeaselProfile, &enabled);
    std::printf("Weasel IsEnabledLanguageProfile HRESULT: 0x%08X enabled=%d\n",
                static_cast<unsigned>(is_enabled), enabled ? 1 : 0);

    HRESULT enable = profiles->EnableLanguageProfile(
        kWeaselClsid, 0x0404, kWeaselProfile, TRUE);
    std::printf("EnableLanguageProfile HRESULT: 0x%08X\n",
                static_cast<unsigned>(enable));
    HRESULT enable_default = profiles->EnableLanguageProfileByDefault(
        kWeaselClsid, 0x0404, kWeaselProfile, TRUE);
    std::printf("EnableLanguageProfileByDefault HRESULT: 0x%08X\n",
                static_cast<unsigned>(enable_default));

    HRESULT activate_language = profiles->ActivateLanguageProfile(
        kWeaselClsid, 0x0404, kWeaselProfile);
    std::printf("Weasel ActivateLanguageProfile HRESULT: 0x%08X\n",
                static_cast<unsigned>(activate_language));

    const GUID ms_bopomofo_clsid = {
        0xB115690A, 0xEA02, 0x48D5,
        {0xA2, 0x31, 0xE3, 0x57, 0x8D, 0x2F, 0xDF, 0x80}};
    const GUID ms_bopomofo_profile = {
        0xB2F9C502, 0x1742, 0x11D4,
        {0x97, 0x90, 0x00, 0x80, 0xC8, 0x82, 0x68, 0x7E}};
    BOOL control_enabled = FALSE;
    HRESULT control_is_enabled = profiles->IsEnabledLanguageProfile(
        ms_bopomofo_clsid, 0x0404, ms_bopomofo_profile,
        &control_enabled);
    std::printf("Bopomofo IsEnabledLanguageProfile HRESULT: 0x%08X enabled=%d\n",
                static_cast<unsigned>(control_is_enabled),
                control_enabled ? 1 : 0);
    HRESULT control_activate = profiles->ActivateLanguageProfile(
        ms_bopomofo_clsid, 0x0404, ms_bopomofo_profile);
    std::printf("Bopomofo ActivateLanguageProfile HRESULT: 0x%08X\n",
                static_cast<unsigned>(control_activate));
    profiles->Release();
  }

  TF_INPUTPROCESSORPROFILE profile_info = {};
  HRESULT get_weasel = mgr->GetProfile(
      TF_PROFILETYPE_INPUTPROCESSOR, 0x0404, kWeaselClsid,
      kWeaselProfile, nullptr, &profile_info);
  std::printf("Weasel ProfileMgr GetProfile HRESULT: 0x%08X\n",
              static_cast<unsigned>(get_weasel));
  HRESULT activate_weasel = mgr->ActivateProfile(
      TF_PROFILETYPE_INPUTPROCESSOR, 0x0404, kWeaselClsid,
      kWeaselProfile, nullptr, TF_IPPMF_FORSESSION);
  std::printf("Weasel ProfileMgr ActivateProfile HRESULT: 0x%08X\n",
              static_cast<unsigned>(activate_weasel));

  IUnknown* weasel_object = nullptr;
  HRESULT cocreate_weasel = CoCreateInstance(
      kWeaselClsid, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown,
      reinterpret_cast<void**>(&weasel_object));
  std::printf("Weasel COM CoCreateInstance HRESULT: 0x%08X\n",
              static_cast<unsigned>(cocreate_weasel));
  if (weasel_object) {
    ITfTextInputProcessorEx* processor = nullptr;
    HRESULT query_processor = weasel_object->QueryInterface(
        IID_ITfTextInputProcessorEx,
        reinterpret_cast<void**>(&processor));
    std::printf("Weasel QI ITfTextInputProcessorEx HRESULT: 0x%08X\n",
                static_cast<unsigned>(query_processor));
    if (processor)
      processor->Release();
    weasel_object->Release();
  }

  ITfTextInputProcessorEx* processor = nullptr;
  HRESULT create_processor = CoCreateInstance(
      kWeaselClsid, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfTextInputProcessorEx,
      reinterpret_cast<void**>(&processor));
  std::printf("Weasel COM create ITfTextInputProcessorEx HRESULT: 0x%08X\n",
              static_cast<unsigned>(create_processor));
  if (processor) {
    ITfThreadMgr* thread_mgr = nullptr;
    HRESULT create_thread_mgr = CoCreateInstance(
        CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfThreadMgr, reinterpret_cast<void**>(&thread_mgr));
    std::printf("ITfThreadMgr CoCreate HRESULT: 0x%08X\n",
                static_cast<unsigned>(create_thread_mgr));
    if (thread_mgr) {
      TfClientId client_id = TF_CLIENTID_NULL;
      HRESULT activate_thread_mgr = thread_mgr->Activate(&client_id);
      std::printf("ITfThreadMgr Activate HRESULT: 0x%08X client_id=%lu\n",
                  static_cast<unsigned>(activate_thread_mgr),
                  static_cast<unsigned long>(client_id));
      if (SUCCEEDED(activate_thread_mgr)) {
        HRESULT activate_processor =
            processor->ActivateEx(thread_mgr, client_id, 0);
        std::printf("Weasel ITfTextInputProcessorEx ActivateEx HRESULT: 0x%08X\n",
                    static_cast<unsigned>(activate_processor));
        if (SUCCEEDED(activate_processor)) {
          HRESULT deactivate_processor = processor->Deactivate();
          std::printf("Weasel ITfTextInputProcessor Deactivate HRESULT: 0x%08X\n",
                      static_cast<unsigned>(deactivate_processor));
        }
        HRESULT deactivate_thread_mgr = thread_mgr->Deactivate();
        std::printf("ITfThreadMgr Deactivate HRESULT: 0x%08X\n",
                    static_cast<unsigned>(deactivate_thread_mgr));
      }
      thread_mgr->Release();
    }
    processor->Release();
  }

  ITfTextInputProcessor* control_processor = nullptr;
  HRESULT create_control_processor = CoCreateInstance(
      kBopomofoClsid, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfTextInputProcessor,
      reinterpret_cast<void**>(&control_processor));
  std::printf("Bopomofo COM create ITfTextInputProcessor HRESULT: 0x%08X\n",
              static_cast<unsigned>(create_control_processor));
  if (control_processor) {
    ITfThreadMgr* control_thread_mgr = nullptr;
    HRESULT create_control_thread_mgr = CoCreateInstance(
        CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfThreadMgr,
        reinterpret_cast<void**>(&control_thread_mgr));
    std::printf("Bopomofo ITfThreadMgr CoCreate HRESULT: 0x%08X\n",
                static_cast<unsigned>(create_control_thread_mgr));
    if (control_thread_mgr) {
      TfClientId control_client_id = TF_CLIENTID_NULL;
      HRESULT activate_control_thread_mgr =
          control_thread_mgr->Activate(&control_client_id);
      std::printf("Bopomofo ITfThreadMgr Activate HRESULT: 0x%08X client_id=%lu\n",
                  static_cast<unsigned>(activate_control_thread_mgr),
                  static_cast<unsigned long>(control_client_id));
      if (SUCCEEDED(activate_control_thread_mgr)) {
        HRESULT activate_control_processor = control_processor->Activate(
            control_thread_mgr, control_client_id);
        std::printf("Bopomofo ITfTextInputProcessor Activate HRESULT: 0x%08X\n",
                    static_cast<unsigned>(activate_control_processor));
        if (SUCCEEDED(activate_control_processor)) {
          HRESULT deactivate_control_processor =
              control_processor->Deactivate();
          std::printf("Bopomofo ITfTextInputProcessor Deactivate HRESULT: 0x%08X\n",
                      static_cast<unsigned>(deactivate_control_processor));
        }
        control_thread_mgr->Deactivate();
      }
      control_thread_mgr->Release();
    }
    control_processor->Release();
  }

  ITfTextInputProcessorEx* diagnostic_processor = nullptr;
  HRESULT create_diagnostic_processor = CoCreateInstance(
      kWeaselClsid, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfTextInputProcessorEx,
      reinterpret_cast<void**>(&diagnostic_processor));
  std::printf("Diag Weasel processor CoCreate HRESULT: 0x%08X\n",
              static_cast<unsigned>(create_diagnostic_processor));
  if (diagnostic_processor) {
    ITfThreadMgr* diagnostic_thread_mgr = nullptr;
    HRESULT create_diagnostic_thread_mgr = CoCreateInstance(
        CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfThreadMgr,
        reinterpret_cast<void**>(&diagnostic_thread_mgr));
    std::printf("Diag ThreadMgr CoCreate HRESULT: 0x%08X\n",
                static_cast<unsigned>(create_diagnostic_thread_mgr));
    if (diagnostic_thread_mgr) {
      TfClientId diagnostic_client_id = TF_CLIENTID_NULL;
      HRESULT activate_diagnostic_thread_mgr =
          diagnostic_thread_mgr->Activate(&diagnostic_client_id);
      std::printf("Diag ThreadMgr Activate HRESULT: 0x%08X client_id=%lu\n",
                  static_cast<unsigned>(activate_diagnostic_thread_mgr),
                  static_cast<unsigned long>(diagnostic_client_id));
      if (SUCCEEDED(activate_diagnostic_thread_mgr)) {
        ITfSource* source = nullptr;
        HRESULT source_hr = diagnostic_thread_mgr->QueryInterface(
            IID_ITfSource, reinterpret_cast<void**>(&source));
        std::printf("Diag QI ITfSource HRESULT: 0x%08X\n",
                    static_cast<unsigned>(source_hr));
        if (source) {
          ITfThreadMgrEventSink* event_sink = nullptr;
          HRESULT sink_hr = diagnostic_processor->QueryInterface(
              IID_ITfThreadMgrEventSink,
              reinterpret_cast<void**>(&event_sink));
          std::printf("Diag QI ITfThreadMgrEventSink HRESULT: 0x%08X\n",
                      static_cast<unsigned>(sink_hr));
          if (event_sink) {
            DWORD cookie = TF_INVALID_COOKIE;
            HRESULT advise = source->AdviseSink(
                IID_ITfThreadMgrEventSink, event_sink, &cookie);
            std::printf("Diag Advise ITfThreadMgrEventSink HRESULT: 0x%08X cookie=%lu\n",
                        static_cast<unsigned>(advise),
                        static_cast<unsigned long>(cookie));
            if (SUCCEEDED(advise))
              source->UnadviseSink(cookie);
            event_sink->Release();
          }

          ITfThreadFocusSink* focus_sink = nullptr;
          HRESULT focus_sink_hr = diagnostic_processor->QueryInterface(
              IID_ITfThreadFocusSink,
              reinterpret_cast<void**>(&focus_sink));
          std::printf("Diag QI ITfThreadFocusSink HRESULT: 0x%08X\n",
                      static_cast<unsigned>(focus_sink_hr));
          if (focus_sink) {
            DWORD cookie = TF_INVALID_COOKIE;
            HRESULT advise = source->AdviseSink(
                IID_ITfThreadFocusSink, focus_sink, &cookie);
            std::printf("Diag Advise ITfThreadFocusSink HRESULT: 0x%08X cookie=%lu\n",
                        static_cast<unsigned>(advise),
                        static_cast<unsigned long>(cookie));
            if (SUCCEEDED(advise))
              source->UnadviseSink(cookie);
            focus_sink->Release();
          }
          source->Release();
        }

        ITfKeystrokeMgr* keystroke_mgr = nullptr;
        HRESULT keystroke_hr = diagnostic_thread_mgr->QueryInterface(
            IID_ITfKeystrokeMgr,
            reinterpret_cast<void**>(&keystroke_mgr));
        std::printf("Diag QI ITfKeystrokeMgr HRESULT: 0x%08X\n",
                    static_cast<unsigned>(keystroke_hr));
        if (keystroke_mgr) {
          ITfKeyEventSink* key_sink = nullptr;
          HRESULT key_sink_hr = diagnostic_processor->QueryInterface(
              IID_ITfKeyEventSink,
              reinterpret_cast<void**>(&key_sink));
          std::printf("Diag QI ITfKeyEventSink HRESULT: 0x%08X\n",
                      static_cast<unsigned>(key_sink_hr));
          if (key_sink) {
            HRESULT advise = keystroke_mgr->AdviseKeyEventSink(
                diagnostic_client_id, key_sink, TRUE);
            std::printf("Diag AdviseKeyEventSink HRESULT: 0x%08X\n",
                        static_cast<unsigned>(advise));
            if (SUCCEEDED(advise))
              keystroke_mgr->UnadviseKeyEventSink(diagnostic_client_id);
            key_sink->Release();
          }
          keystroke_mgr->Release();
        }

        ITfLangBarItemMgr* langbar_mgr = nullptr;
        HRESULT langbar_hr = diagnostic_thread_mgr->QueryInterface(
            IID_ITfLangBarItemMgr,
            reinterpret_cast<void**>(&langbar_mgr));
        std::printf("Diag QI ITfLangBarItemMgr HRESULT: 0x%08X\n",
                    static_cast<unsigned>(langbar_hr));
        if (langbar_mgr) {
          const GUID langbar_guid = {
              0x2C77A81E, 0x41CC, 0x4178,
              {0xA3, 0xA7, 0x5F, 0x8A, 0x98, 0x75, 0x68, 0xE6}};
          ITfLangBarItem* existing_item = nullptr;
          HRESULT get_item = langbar_mgr->GetItem(
              langbar_guid, &existing_item);
          std::printf("Diag LangBar GetItem(Weasel GUID) HRESULT: 0x%08X existing=%d\n",
                      static_cast<unsigned>(get_item),
                      existing_item ? 1 : 0);
          if (existing_item)
            existing_item->Release();
          langbar_mgr->Release();
        }

        ITfCompartmentMgr* compartment_mgr = nullptr;
        HRESULT compartment_hr = diagnostic_thread_mgr->QueryInterface(
            IID_ITfCompartmentMgr,
            reinterpret_cast<void**>(&compartment_mgr));
        std::printf("Diag QI ITfCompartmentMgr HRESULT: 0x%08X\n",
                    static_cast<unsigned>(compartment_hr));
        if (compartment_mgr) {
          ITfCompartment* compartment = nullptr;
          HRESULT get_compartment = compartment_mgr->GetCompartment(
              GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &compartment);
          std::printf("Diag Get keyboard compartment HRESULT: 0x%08X\n",
                      static_cast<unsigned>(get_compartment));
          if (compartment)
            compartment->Release();
          compartment_mgr->Release();
        }

        diagnostic_thread_mgr->Deactivate();
      }
      diagnostic_thread_mgr->Release();
    }
    diagnostic_processor->Release();
  }

  std::puts("=== ProfileMgr after EnableLanguageProfile ===");
  dump_profiles(mgr);

  HMODULE input = LoadLibraryW(L"input.dll");
  std::printf("LoadLibrary(input.dll): %s\n", input ? "ok" : "failed");
  if (input) {
    using InstallLayoutOrTipFn = BOOL(WINAPI*)(LPCWSTR, DWORD);
    using InstallLayoutOrTipUserRegFn = BOOL(WINAPI*)(
        LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, DWORD);
    using QueryLayoutOrTipStringUserRegFn = HRESULT(WINAPI*)(
        LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, DWORD);
    auto install_layout_or_tip = reinterpret_cast<InstallLayoutOrTipFn>(
        GetProcAddress(input, "InstallLayoutOrTip"));
    auto install_layout_or_tip_user_reg =
        reinterpret_cast<InstallLayoutOrTipUserRegFn>(
            GetProcAddress(input, "InstallLayoutOrTipUserReg"));
    auto query_layout_or_tip_user_reg =
        reinterpret_cast<QueryLayoutOrTipStringUserRegFn>(
            GetProcAddress(input, "QueryLayoutOrTipStringUserReg"));
    std::printf("GetProcAddress(InstallLayoutOrTip): %s\n",
                install_layout_or_tip ? "ok" : "failed");
    std::printf("GetProcAddress(InstallLayoutOrTipUserReg): %s\n",
                install_layout_or_tip_user_reg ? "ok" : "failed");
    std::printf("GetProcAddress(QueryLayoutOrTipStringUserReg): %s\n",
                query_layout_or_tip_user_reg ? "ok" : "failed");
    const WCHAR tip[] =
        L"0404:{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}{3D02CAB6-2B8E-4781-BA20-1C9267529467}";
    if (query_layout_or_tip_user_reg) {
      HRESULT valid = query_layout_or_tip_user_reg(
          nullptr, nullptr, nullptr, tip, 0);
      std::printf("QueryLayoutOrTipStringUserReg(default roots) HRESULT: 0x%08X\n",
                  static_cast<unsigned>(valid));
    }
    if (install_layout_or_tip_user_reg) {
      BOOL installed_user_reg = install_layout_or_tip_user_reg(
          nullptr, nullptr, nullptr, tip, 0);
      std::printf("InstallLayoutOrTipUserReg(default roots) result: %d last_error=%lu\n",
                  installed_user_reg ? 1 : 0,
                  static_cast<unsigned long>(GetLastError()));
    }
    if (query_layout_or_tip_user_reg) {
      HRESULT valid_after_user_reg = query_layout_or_tip_user_reg(
          nullptr, nullptr, nullptr, tip, 0);
      std::printf("QueryLayoutOrTipStringUserReg(after install) HRESULT: 0x%08X\n",
                  static_cast<unsigned>(valid_after_user_reg));
    }
    if (install_layout_or_tip) {
      BOOL installed = install_layout_or_tip(tip, 0);
      std::printf("InstallLayoutOrTip result: %d last_error=%lu\n",
                  installed ? 1 : 0,
                  static_cast<unsigned long>(GetLastError()));
    }
    FreeLibrary(input);
  }

  std::puts("=== ProfileMgr after InstallLayoutOrTip ===");
  dump_profiles(mgr);
}

int wmain(int argc, wchar_t** argv) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr))
    return 2;

  ITfInputProcessorProfileMgr* mgr = nullptr;
  hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                        CLSCTX_INPROC_SERVER,
                        IID_ITfInputProcessorProfileMgr,
                        reinterpret_cast<void**>(&mgr));
  if (FAILED(hr)) {
    std::printf("CoCreateInstance failed: 0x%08X\n",
                static_cast<unsigned>(hr));
    CoUninitialize();
    return 3;
  }

  const bool register_mode =
      argc > 1 && std::wcscmp(argv[1], L"register") == 0;
  const bool inspect_mode =
      argc > 1 && std::wcscmp(argv[1], L"inspect") == 0;
  const bool activate_mode =
      argc > 1 && std::wcscmp(argv[1], L"activate") == 0;

  if (inspect_mode || activate_mode) {
    print_identity();
    std::puts("=== User install state ===");
    print_user_install_state();
    std::puts("=== Registration keys in current user ===");
    print_registration_keys();
    std::puts("=== ProfileMgr in current user ===");
  }

  if (activate_mode) {
    probe_hklm_override_registration(mgr);
    probe_user_activation(mgr);
  }

  if (register_mode) {
    print_identity();
    std::puts("=== Registration keys before TSF registration ===");
    print_registration_keys();

    ITfInputProcessorProfiles* profiles = nullptr;
    HRESULT register_create = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfiles,
        reinterpret_cast<void**>(&profiles));
    std::printf("ITfInputProcessorProfiles CoCreate HRESULT: 0x%08X\n",
                static_cast<unsigned>(register_create));
    if (SUCCEEDED(register_create)) {
      HRESULT register_service = profiles->Register(kWeaselClsid);
      std::printf("Register text service HRESULT: 0x%08X\n",
                  static_cast<unsigned>(register_service));
      profiles->Release();
    }

    std::puts("=== Registration keys after Register(text service) ===");
    print_registration_keys();

    WCHAR icon_file[MAX_PATH] = {};
    ULONG icon_len = GetModuleFileNameW(nullptr, icon_file,
                                        ARRAYSIZE(icon_file));
    const WCHAR description[] = L"Weasel";
    hr = mgr->RegisterProfile(
        kWeaselClsid, 0x0404, kWeaselProfile, description,
        (ARRAYSIZE(description) - 1) * sizeof(wchar_t), icon_file,
        icon_len, 0, nullptr, 0, TRUE, 0);
    std::printf("RegisterProfile HRESULT: 0x%08X\n",
                static_cast<unsigned>(hr));

    std::puts("=== Registration keys after RegisterProfile ===");
    print_registration_keys();

    ITfCategoryMgr* category_mgr = nullptr;
    HRESULT category_create = CoCreateInstance(
        CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfCategoryMgr, reinterpret_cast<void**>(&category_mgr));
    std::printf("ITfCategoryMgr CoCreate HRESULT: 0x%08X\n",
                static_cast<unsigned>(category_create));
    if (SUCCEEDED(category_create)) {
      HRESULT register_category = category_mgr->RegisterCategory(
          kWeaselClsid, GUID_TFCAT_CATEGORY_OF_TIP, kWeaselClsid);
      std::printf("RegisterCategory HRESULT: 0x%08X\n",
                  static_cast<unsigned>(register_category));
      category_mgr->Release();
    }

    std::puts("=== Registration keys after RegisterCategory ===");
    print_registration_keys();
    std::puts("=== ProfileMgr after RegisterProfile ===");
  }

  dump_profiles(mgr);
  mgr->Release();
  CoUninitialize();
  return 0;
}
