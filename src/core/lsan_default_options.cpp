#if defined(__SANITIZE_ADDRESS__)
#define VSTREAMER_WITH_ASAN 1
#elif defined(__clang__)
#if __has_feature(address_sanitizer)
#define VSTREAMER_WITH_ASAN 1
#endif
#endif

#if defined(VSTREAMER_WITH_ASAN)

#ifndef VSTREAMER_LSAN_SUPPRESSIONS_PATH
#error "VSTREAMER_LSAN_SUPPRESSIONS_PATH must be set for ASAN builds"
#endif

extern "C" const char *__lsan_default_options()
{
    return "suppressions=" VSTREAMER_LSAN_SUPPRESSIONS_PATH ":print_suppressions=0";
}

#endif
