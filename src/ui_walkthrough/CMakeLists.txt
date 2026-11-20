fv_module(KIND feature
          DEPS fv_base fv_pyport fv_settings fv_uimath fv_walkthrough fv_ui_snap fv_ui_widgets
               fv_ui_theme fv_appkit fv_agents fv_ui_shell fv_ui_history fv_ui_overlay fv_dictation
               fv_documents fv_focus fv_clipboard fv_hotkeys fv_recorder fv_appcore fv_ui_settings)
fv_feature(ID walkthrough CLASS fv::WalkthroughFeature HEADER ui_walkthrough/walkthrough_feature.h)
