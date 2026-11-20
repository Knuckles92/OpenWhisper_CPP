fv_module(KIND core
          DEPS fv_automation fv_remote fv_credentials fv_process fv_history fv_net fv_db fv_sql
               fv_settings fv_pyport fv_base)
fv_embed(data/openapi.json data/mcp_tools.json data/mcp_event_schemas.json data/live_openapi.json)
