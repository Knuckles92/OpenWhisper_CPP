fv_module(KIND core
          DEPS fv_download fv_archive fv_engine fv_history fv_settings fv_pyport fv_base
          PRIVATE_THIRD_PARTY ${CMAKE_DL_LIBS})
fv_embed(data/speech_catalog.json data/onnxruntime_catalog.json data/pdfium_catalog.json)
