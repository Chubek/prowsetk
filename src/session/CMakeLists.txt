set(PROWSETK_SESSION_SOURCES
    tokyocabinet_storage.cpp
    encrypted_storage.cpp
    storage_factory.cpp
)

prowsetk_add_library(prowsetk_session ${PROWSETK_SESSION_SOURCES})
set_target_properties(prowsetk_session PROPERTIES EXPORT_NAME session)

target_include_directories(prowsetk_session PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/third_party/Tokyo-Cabinet"
)

target_link_libraries(prowsetk_session PUBLIC ProwseTk::core)
target_link_libraries(prowsetk_session PRIVATE
    ProwseTk::tokyocabinet
    ProwseTk::tomcrypt)

add_library(ProwseTk::session ALIAS prowsetk_session)

prowsetk_install_library(prowsetk_session)
