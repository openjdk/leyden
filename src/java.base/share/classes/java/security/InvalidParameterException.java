/*
 * Copyright (c) 1996, 2022, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package java.security;

/**
 * This exception, designed for use by the JCA/JCE engine classes,
 * is thrown when an invalid parameter is passed
 * to a method.
 *
 * @author Benjamin Renaud
 * @since 1.1
 */

public class InvalidParameterException extends IllegalArgumentException {

    @java.io.Serial
    private static final long serialVersionUID = -857968536935667808L;

    /**
     * Constructs an {@code InvalidParameterException} with no detail message.
     * A detail message is a {@code String} that describes this particular
     * exception.
     */
    public InvalidParameterException() {
        super();
    }

    /**
     * Constructs an {@code InvalidParameterException} with the specified
     * detail message.  A detail message is a {@code String} that describes
     * this particular exception.
     *
     * @param msg the detail message.
     */
    public InvalidParameterException(String msg) {
        super(msg);
    }

    /**
     * Constructs an {@code InvalidParameterException} with the specified
     * detail message and cause. A detail message is a {@code String}
     * that describes this particular exception.
     *
     * <p>Note that the detail message associated with {@code cause} is
     * <i>not</i> automatically incorporated in this exception's detail
     * message.
     *
     * @param  msg the detail message (which is saved for later retrieval
     *         by the {@link Throwable#getMessage()} method).
     * @param  cause the cause (which is saved for later retrieval by the
     *         {@link Throwable#getCause()} method). (A {@code null} value
     *         is permitted, and indicates that the cause is nonexistent or
     *         unknown.)
     *
     * @since  20
     */
    public InvalidParameterException(String msg, Throwable cause) {
        super(msg, cause);
    }

    /**
     * Constructs an {@code InvalidParameterException} with the specified cause
     * and a detail message of {@code (cause==null ? null : cause.toString())}
     * (which typically contains the class and detail message of {@code cause}).
     * This constructor is useful for exceptions that are little more than
     * wrappers for other throwables.
     *
     * @param  cause the cause (which is saved for later retrieval by the
     *         {@link Throwable#getCause()} method). (A {@code null} value is
     *         permitted, and indicates that the cause is nonexistent or
     *         unknown.)
     *
     * @since  20
     */
    public InvalidParameterException(Throwable cause) {
        super(cause);
    }
}
