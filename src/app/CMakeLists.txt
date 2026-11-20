# openwhisper-cpp.exe: the windowed desktop app. Everything but main() is in fv_ui_shell and the
# features, which the generated registry links in.
fv_module(KIND exe NAME openwhisper-cpp GUI FEATURES
          DEPS fv_ui_shell fv_appkit fv_base fv_engine fv_backup fv_update fv_update_native fv_process fv_http)
