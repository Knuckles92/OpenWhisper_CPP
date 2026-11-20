fv_module(KIND feature
          DEPS fv_base fv_settings fv_engine fv_models fv_history fv_appkit fv_ui_theme
               fv_ui_widgets fv_appcore fv_hotkeys fv_recorder fv_ui_hotkeys fv_ui_record
               fv_ui_settings fv_ui_downloads fv_ui_shell fv_update fv_uimath fv_backup
               fv_credentials fv_llm fv_ui_personalize fv_agents)
fv_feature(ID setup CLASS fv::SetupFeature HEADER ui_setup/setup_feature.h)
