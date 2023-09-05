package jdk.internal.constant;

enum State {
    UNBOUND,
    NON_NULL,
    NULL,
    ERROR;

    byte ordinalAsByte() {
        return (byte) ordinal();
    }

    // This or values()[value]
    static State of(byte value) {
        return switch (value) {
            case 0 -> UNBOUND;
            case 1 -> NON_NULL;
            case 2 -> NULL;
            case 3 -> ERROR;
            default -> throw new InternalError(Byte.toString(value));
        };
    }

}
