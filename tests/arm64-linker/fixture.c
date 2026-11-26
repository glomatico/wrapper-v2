static int initialized;

__attribute__((constructor)) static void initialize(void) {
    initialized = 42;
}

int wrapper_linker_fixture_value(void) {
    return initialized;
}
