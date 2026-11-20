fv_module(KIND core
          DEPS fv_base fv_pyport
          PRIVATE_THIRD_PARTY Boost::beast Boost::asio OpenSSL::SSL OpenSSL::Crypto
          WIN32_LIBS ws2_32 mswsock iphlpapi)

# Beast's WebSocket and TLS templates, instantiated for plain and TLS streams in one file, pass
# the COFF limit of 65,279 sections.
if(MSVC)
    target_compile_options(fv_net PRIVATE /bigobj)
endif()
