fv_test(NO_MAIN QT Core Gui Widgets DEPS fv_testkit fv_testkit_qt fv_golden fv_walkthrough fv_base)

# The PDFium reader lives in a subfolder, which fv_test does not glob.
if(TARGET fv_documents_tests)
    target_sources(fv_documents_tests PRIVATE support/pdfium_reader.h support/pdfium_reader.cpp)
    target_include_directories(fv_documents_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
