namespace {
[[gnu::noinline]] void raiseControl() {
    throw 403;
}
}

extern "C" int fleet_probe_exception() {
    try {
        raiseControl();
    } catch (int value) {
        return value == 403;
    }
    return 0;
}
