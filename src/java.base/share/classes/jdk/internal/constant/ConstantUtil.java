package jdk.internal.constant;

import jdk.internal.misc.Unsafe;

import java.lang.invoke.VarHandle;

final class ConstantUtil {

    private ConstantUtil() {
    }

    /**
     * Performs a "freeze" operation, required to ensure safe publication under plain memory read
     * semantics.
     * <p>
     * This inserts a memory barrier, thereby establishing a happens-before constraint. This
     * prevents the reordering of store operations across the freeze boundary.
     */
    static void freeze() {
        // Issue a store fence, which is sufficient
        // to provide protection against store/store reordering.
        VarHandle.releaseFence();
    }

    static long offset(Class<?> c, String name) {
        return Unsafe.getUnsafe().objectFieldOffset(c, name);
    }

}
