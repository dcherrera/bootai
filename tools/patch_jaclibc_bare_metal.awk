#!/usr/bin/awk -f
#
# patch_jaclibc_bare_metal.awk
#
# Patches JACLibc include/config.h for bare-metal (freestanding) compatibility.
#
# Fixes:
#
# 1. JACL_HAS_POSIX: Bare-metal targets (JACL_OS_NONE defined via os/detect.h
#    when __STDC_HOSTED__==0) incorrectly get JACL_HAS_POSIX=1 because the
#    check only excludes Windows. This causes __jacl_grow_heap() in stdlib to
#    call mmap(), which doesn't exist on bare metal.
#    Fix: also check for JACL_OS_NONE.
#
# 2. JACL_HAS_PTHREADS: Depends on JACL_HAS_POSIX, but is defined before
#    the fix could take effect. Already correct after fix 1.
#
# Usage:
#   awk -f tools/patch_jaclibc_bare_metal.awk \
#       JACLibc/include/config.h > /tmp/config_patched.h
#   mv /tmp/config_patched.h JACLibc/include/config.h
#

# Fix: JACL_HAS_POSIX — exclude bare metal (JACL_OS_NONE)
#
# Original (lines 333-338):
#   #ifdef JACL_OS_WINDOWS
#     #define JACL_HAS_POSIX 0
#   #else /* everything else */
#     #define JACL_HAS_POSIX 1
#   #endif /* posix check */
#
# Patched:
#   #if defined(JACL_OS_WINDOWS) || (defined(JACL_OS_NONE) || (defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 0))
#     #define JACL_HAS_POSIX 0
#   #else /* everything else */
#     #define JACL_HAS_POSIX 1
#   #endif /* posix check */

/^#ifdef JACL_OS_WINDOWS$/ {
    # Peek at next line to confirm this is the POSIX block
    if (getline next_line > 0) {
        if (next_line ~ /JACL_HAS_POSIX/) {
            print "#if defined(JACL_OS_WINDOWS) || defined(JACL_OS_NONE) || (defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 0)"
            print next_line
            next
        } else {
            # Not the POSIX block, print both lines normally
            print
            print next_line
            next
        }
    }
}

# Default: pass through unchanged
{ print }
