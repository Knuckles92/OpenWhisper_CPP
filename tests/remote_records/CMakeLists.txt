# fv_remote_host: test_sync.cpp and test_last_dictation.cpp send records to a real host service on
# 127.0.0.1 (sync_rig.h). fv_backup: test_host_restore.cpp carries a host's data root by backup.
fv_test(DEPS fv_testkit fv_meeting_state fv_remote_host fv_backup Boost::asio OpenSSL::SSL OpenSSL::Crypto)
