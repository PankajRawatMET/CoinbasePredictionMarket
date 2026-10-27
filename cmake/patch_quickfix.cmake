# Patch applied to the QuickFIX v1.16.0 sources at fetch time (cwd = source dir).
#
# QuickFIX's SSL initiator sets SNI but never checks that the server certificate
# matches the host name: with CertificationAuthoritiesFile set it verifies the
# chain only, so any CA-signed certificate would be accepted. SSL_set1_host makes
# OpenSSL check the certificate's SAN/CN against the host during the handshake.
set(file "src/C++/ThreadedSSLSocketInitiator.cpp")
set(anchor "    ssl_set_sni_hostname(ssl, host.address, log);\n")
set(addition "    // CPM patch: verify the certificate matches the host name.\n    SSL_set1_host(ssl, host.address.c_str());\n")

file(READ "${file}" content)
string(FIND "${content}" "CPM patch" already)
if(already GREATER -1)
  return()
endif()
string(FIND "${content}" "${anchor}" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "patch_quickfix: anchor not found in ${file}; QuickFIX source changed")
endif()
string(REPLACE "${anchor}" "${anchor}${addition}" content "${content}")
file(WRITE "${file}" "${content}")
message(STATUS "patch_quickfix: added TLS hostname verification to ${file}")
