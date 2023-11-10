#Description: MbedTLS Adaptation for On-chip RT Crypto accelerator.; user_visible: True
include_guard(GLOBAL)
message("middleware_mbedtls_rt component is included.")

target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE
)

target_include_directories(${MCUX_SDK_PROJECT_NAME} PUBLIC
)


include(driver_dcp)
include(middleware_mbedtls_port_ksdk)
include(driver_trng)