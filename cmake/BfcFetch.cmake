include(FetchContent)

# Header-only; only src/bfc is used.
FetchContent_Declare(bfc
    GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG 6c02a495d1a2cf4a8a13dfa6b25b342780527c89
)
FetchContent_GetProperties(bfc)
if(NOT bfc_POPULATED)
    FetchContent_Populate(bfc)
endif()
