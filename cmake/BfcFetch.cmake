include(FetchContent)

# Header-only; only src/bfc is used.
FetchContent_Declare(bfc
    GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG 8bb42d93baf316a7bbd8ef6f25cd54b2747c4f13
)
FetchContent_GetProperties(bfc)
if(NOT bfc_POPULATED)
    FetchContent_Populate(bfc)
endif()
