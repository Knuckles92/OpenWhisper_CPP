# fv_meeting_capture: test_meeting_remote.cpp runs the meeting's ASR worker over a paired host.
fv_test(DEPS fv_testkit fv_golden fv_remote_wire_testkit fv_meeting_state fv_meeting_capture
             Boost::asio OpenSSL::SSL OpenSSL::Crypto)
