# pocketfft: the tone tests find a dominant frequency the way OpenWhisper's do, with an rfft.
fv_test(DEPS fv_testkit fv_golden pocketfft::pocketfft)
