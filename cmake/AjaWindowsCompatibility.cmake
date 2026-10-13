# SDK 18.0.0's WMI helper jumps over initialized variables, rejected by MSVC.
# Compile a corrected build-directory copy, preserving the downloaded SDK.
set(_aja_info "${AJA_SDK_DIR}/ajabase/system/windows/infoimpl.cpp")
file(READ "${_aja_info}" _aja_source)
set(_aja_declarations
    "IWbemServices *pSvc = NULL"
    "IClientSecurity* pSecurity = NULL"
    "DWORD authnSvc = 0"
    "DWORD authzSvc = 0"
    "LPOLESTR serverPrincName = NULL"
    "DWORD authnLevel = 0"
    "DWORD impLevel = 0"
    "RPC_AUTH_IDENTITY_HANDLE authInfo = NULL"
    "DWORD ifCapabilites = 0"
    "IEnumWbemClassObject* pEnumerator = NULL"
    "IWbemClassObject *pClsObj = NULL"
    "ULONG uReturn = 0")
set(_aja_prefix "")
foreach(_declaration IN LISTS _aja_declarations)
    string(REPLACE "${_declaration};" "" _aja_source "${_aja_source}")
    string(APPEND _aja_prefix "\n\t${_declaration};")
endforeach()
string(REPLACE "std::string retVal = \"Microsoft Windows\";"
    "std::string retVal = \"Microsoft Windows\";${_aja_prefix}" _aja_source "${_aja_source}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/aja-infoimpl.cpp" "${_aja_source}")
get_target_property(_aja_sources ajantv2 SOURCES)
list(FILTER _aja_sources EXCLUDE REGEX "ajabase/system/windows/infoimpl\\.cpp$")
set_property(TARGET ajantv2 PROPERTY SOURCES "${_aja_sources}")
target_sources(ajantv2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/aja-infoimpl.cpp")
target_include_directories(ajantv2 PRIVATE "${AJA_SDK_DIR}/ajabase/system/windows")
