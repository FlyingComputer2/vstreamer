include(FetchContent)

# Header-only; only src/bfc is used.
FetchContent_Declare(bfc
    GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG f6fe9784bb8379eaa72a1597fe9e9638b97fb5fe
)
FetchContent_GetProperties(bfc)
if(NOT bfc_POPULATED)
    FetchContent_Populate(bfc)
endif()
