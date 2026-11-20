fv_module(KIND feature
          DEPS fv_base fv_pyport fv_log fv_settings fv_engine fv_sql fv_db fv_http fv_audio fv_recorder
               fv_models fv_appcore fv_speech fv_hotkeys fv_focus fv_clipboard fv_history fv_dictation
               fv_appkit fv_uimath fv_ui_theme fv_ui_widgets fv_ui_shell fv_ui_record fv_ui_history
               fv_ui_overlay fv_ui_setup fv_ui_settings fv_ui_downloads fv_update fv_update_native fv_backup
               fv_process fv_text fv_llm fv_agents fv_ui_personalize fv_automation
               fv_walkthrough fv_remote)
fv_feature(ID controller CLASS fv::ControllerFeature HEADER ui_controller/controller_feature.h)
