"""Exercise the installed shared ABI at both rates in both shaft modes."""
from ffi_smoke import render


def matrix(library, fixture):
    results = []
    for mode in (0, 1):
        for rate in (44100, 48000):
            events = [(0, .25, 0, 1, 1 if mode == 0 else 0, 600 if mode else 0),
                      (rate, .25, 1 if mode else 0, 1, 0, 600 if mode else 0)]
            pcm, telemetry = render(library, fixture / "main.json", fixture, rate, mode, 17, rate * 2, events,
                                    use_bytes=bool(mode))
            fingerprint = 14695981039346656037
            for value in pcm:
                fingerprint = ((fingerprint ^ value) * 1099511628211) & ((1 << 64) - 1)
            assert telemetry["measured_rpm"] > 100 and telemetry["crank_rad_s"] < 0
            if mode:
                assert abs(telemetry["measured_rpm"] - 600) <= .1
            results.append({"rate": rate, "mode": mode, "frames": telemetry["frames"], "ticks": telemetry["ticks"],
                            "native_frames": telemetry["native_frames"], "pcm_fnv1a": f"{fingerprint:016x}"})
    return results
