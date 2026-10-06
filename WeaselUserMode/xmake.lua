target("WeaselUserMode")
  set_kind("binary")
  add_files("./*.cpp")
  add_files("$(projectdir)/WeaselTSF/KeyEvent.cpp")
  add_rules("subwin")
  add_deps("WeaselIPC")
  add_includedirs("$(projectdir)/WeaselIPC")
  add_links("user32", "shell32", "imm32")
  set_policy("windows.manifest.uac", "invoker")

  after_build(function(target)
    os.cp(path.join(target:targetdir(), "WeaselUserMode.exe"), "$(projectdir)/output")
    if os.exists(path.join(target:targetdir(), "WeaselUserMode.pdb")) then
      os.cp(path.join(target:targetdir(), "WeaselUserMode.pdb"), "$(projectdir)/output")
    end
  end)
