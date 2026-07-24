add_executable(nz_legal
    ../building_consents/main.cpp
    ../building_consents/mimalloc_override.cpp
)

target_compile_definitions(nz_legal PRIVATE ASTRAEA_NZ_LEGAL_APP)

target_link_libraries(nz_legal PRIVATE
    astraea_clients
    nz_legal_routes
    mimalloc
    astraea_warnings
)

option(ASTRAEA_ENABLE_TIMING "Emit per-request timing SSE event on /ask/stream" ON)
if(ASTRAEA_ENABLE_TIMING)
    target_compile_definitions(nz_legal PRIVATE ASTRAEA_ENABLE_TIMING)
endif()
