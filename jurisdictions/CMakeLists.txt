add_library(nz_tenancy_routes STATIC
    nz_tenancy/routes.cpp
    nz_tenancy/jurisdiction.cpp
)
target_include_directories(nz_tenancy_routes PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/jurisdictions
)
target_link_libraries(nz_tenancy_routes PUBLIC astraea_core)

add_library(nz_building_routes STATIC
    building_consents/routes.cpp
    building_consents/jurisdiction.cpp
)
target_include_directories(nz_building_routes PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/jurisdictions
)
target_link_libraries(nz_building_routes PUBLIC astraea_core)

add_library(nz_legal_routes STATIC
    nz_legal/jurisdiction.cpp
)
target_include_directories(nz_legal_routes PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/jurisdictions
)
target_link_libraries(nz_legal_routes PUBLIC astraea_core)
