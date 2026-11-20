fv_module(KIND core
          DEPS fv_base fv_pyport fv_settings fv_meeting_state fv_llm fv_archive fv_meeting_agent
               fv_agents
          PRIVATE_THIRD_PARTY pugixml::pugixml ${CMAKE_DL_LIBS})
