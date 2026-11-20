fv_module(KIND feature
          DEPS fv_base fv_pyport fv_log fv_settings fv_speech fv_uimath fv_appkit fv_ui_theme
               fv_ui_widgets fv_ui_shell fv_appcore fv_audio fv_recorder fv_history fv_credentials
               fv_http fv_hotkeys fv_ui_hotkeys fv_ui_overlay fv_models fv_backup fv_ui_history
               fv_llm fv_agents fv_walkthrough)
fv_feature(ID settings CLASS fv::SettingsFeature HEADER ui_settings/settings_feature.h)
