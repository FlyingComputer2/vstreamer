include(FetchContent)

# Header-only; only src/bfc is used.
FetchContent_Declare(bfc
    GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG b4f3040cccdc544bb2c224cbc5795d5b2ff2f45f
)
FetchContent_GetProperties(bfc)
if(NOT bfc_POPULATED)
    FetchContent_Populate(bfc)
endif()
