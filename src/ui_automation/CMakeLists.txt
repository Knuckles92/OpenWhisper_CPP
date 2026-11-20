fv_module(KIND feature
          DEPS fv_appkit fv_ui_theme fv_ui_widgets fv_ui_hotkeys fv_ui_settings fv_automation
               fv_agentapi fv_agents fv_settings fv_text fv_hotkeys fv_credentials fv_llm fv_http
               fv_pyport fv_base fv_remote fv_meeting fv_ui_history fv_ui_personalize)
fv_feature(ID automation CLASS fv::AutomationFeature
           HEADER ui_automation/automation_feature.h)
