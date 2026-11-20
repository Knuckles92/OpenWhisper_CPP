# NO_MAIN: main.cpp runs Catch2, and doubles as the child process the hard-exit recovery test
# starts.
fv_test(NO_MAIN DEPS fv_testkit fv_golden fv_base fv_pyport fv_audio fv_recorder)
