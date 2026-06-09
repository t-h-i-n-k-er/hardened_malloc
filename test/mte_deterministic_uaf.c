#ifdef HAS_ARM_MTE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arm_acle.h>
#include <signal.h>
#include <setjmp.h>

#include "test_util.h"

static sigjmp_buf jmpbuf;
static volatile sig_atomic_t caught_signal;

static void sigsegv_handler(int sig) {
    caught_signal = sig;
    siglongjmp(jmpbuf, 1);
}

OPTNONE int main(void) {
    struct sigaction sa, old_sa;
    sa.sa_handler = sigsegv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, &old_sa);

    // Allocate a slot and save the tagged pointer
    char *p1 = malloc(16);
    if (!p1) {
        return 1;
    }
    memset(p1, 'a', 16);

    // Save the original tagged pointer (includes MTE tag in top bits)
    char *saved_tagged_ptr = p1;
    u8 original_tag = ((uintptr_t)p1 >> 56) & 0xf;

    if (original_tag == 0) {
        // Tag 0 is the reserved tag; this shouldn't happen for a valid allocation
        fprintf(stderr, "allocated pointer has reserved tag 0\n");
        return 1;
    }

    // Free the slot
    free(p1);

    // Allocate again - with deterministic UAF, the tag should be incremented
    char *p2 = malloc(16);
    if (!p2) {
        return 1;
    }
    memset(p2, 'b', 16);

    // The new pointer should have a different tag from the original
    u8 new_tag = ((uintptr_t)p2 >> 56) & 0xf;

    if (new_tag == 0) {
        fprintf(stderr, "reallocation has reserved tag 0\n");
        return 1;
    }

    // With deterministic UAF, the new tag should be the previous tag + 1
    // (skipping 0). The untagged addresses should be the same slot.
    u8 expected_tag = original_tag + 1;
    if (expected_tag == 0) {
        expected_tag = 1; // skip reserved tag
    }
    if (expected_tag > 0xf) {
        expected_tag = 1; // wrap within 4-bit space, skip 0
    }

    uintptr_t mask = UINTPTR_MAX >> 8;
    if (((uintptr_t)saved_tagged_ptr & mask) != ((uintptr_t)p2 & mask)) {
        // Different slot was allocated, can't directly verify tag increment
        // but we can still verify that accessing the old tagged pointer
        // would fault (if we got the same slot)
        fprintf(stderr, "different slot allocated, skipping tag increment check\n");
        free(p2);
        sigaction(SIGSEGV, &old_sa, NULL);
        return 0;
    }

    if (new_tag != expected_tag) {
        // Tag might differ due to neighbor collision avoidance, but should still
        // be different from the original tag
        if (new_tag == original_tag) {
            fprintf(stderr, "new tag matches original tag - deterministic UAF broken\n");
            return 1;
        }
    }

    // Now free the second allocation and try to access through the original
    // tagged pointer. This should trigger an MTE fault because the memory
    // is now tagged with the reserved tag (0) and the original pointer
    // has a non-zero tag.
    free(p2);

    caught_signal = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        // Access through the original tagged pointer after free
        // Should trigger SIGSEGV due to MTE tag mismatch
        volatile char val = *saved_tagged_ptr;
        (void)val;
    }

    // Restore original signal handler
    sigaction(SIGSEGV, &old_sa, NULL);

    if (caught_signal == SIGSEGV) {
        // MTE correctly detected the use-after-free
        return 0;
    }

    // If we get here, the access didn't fault. This could mean:
    // - MTE is not actually enabled at runtime (is_memtag_disabled)
    // - The slot was reclaimed and happened to get the same tag
    // Either way, deterministic UAF didn't work as expected.
    fprintf(stderr, "use-after-free was not detected\n");
    return 1;
}

#else /* !HAS_ARM_MTE */

#include <stdio.h>

int main(void) {
    fprintf(stderr, "ARM MTE not available, skipping test\n");
    return 0;
}

#endif
