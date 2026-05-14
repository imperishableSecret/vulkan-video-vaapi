#!/usr/bin/env python3

from pathlib import Path
import re
import sys


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    root = Path(sys.argv[1])
    vulkan_root = root / "src" / "vulkan"

    direct_trace = []
    for path in vulkan_root.rglob("*.cpp"):
        text = path.read_text(encoding="utf-8")
        for number, line in enumerate(text.splitlines(), start=1):
            if re.search(r"\bvkvv_trace\(", line):
                direct_trace.append(f"{path.relative_to(root)}:{number}")
    if direct_trace:
        fail("direct vkvv_trace() remains in hot Vulkan sources:\n" + "\n".join(direct_trace))

    shadow = root / "src" / "vulkan" / "export" / "shadow_image.cpp"
    lines = shadow.read_text(encoding="utf-8").splitlines()
    for index, line in enumerate(lines):
        if "export_seed_records_string(" not in line:
            continue
        if line.lstrip().startswith("std::string export_seed_records_string("):
            continue
        window = "\n".join(lines[max(0, index - 4) : index + 1])
        if "vkvv_trace_deep_enabled()" not in window:
            fail(f"export_seed_records_string() is not deep-trace gated at {shadow.relative_to(root)}:{index + 1}")

    av1_decode = root / "src" / "vulkan" / "codecs" / "av1" / "decode.cpp"
    av1_text = av1_decode.read_text(encoding="utf-8")
    if '"av1-display-decision"' not in av1_text:
        fail("AV1 display decision trace is missing")
    for field in (
        "show_frame=",
        "show_existing_frame=",
        "showable_frame=",
        "refresh_frame_flags=",
        "frame_to_show_map_idx=",
        "content_gen=",
        "shadow_gen=",
        "refresh_export=",
        "exported=",
        "shadow_exported=",
        "predecode=",
        "seeded=",
        "last_display_surface=",
        "last_display_gen=",
        "display_action=",
    ):
        if field not in av1_text:
            fail(f"AV1 display decision trace is missing field {field}")

    export_text = (root / "src" / "vulkan" / "export.cpp").read_text(encoding="utf-8")
    if '"av1-visible-output-check"' not in export_text:
        fail("AV1 visible output check trace is missing")
    for field in (
        "show_frame=",
        "show_existing_frame=",
        "refresh_frame_flags=",
        "frame_to_show_map_idx=",
        "refresh_export=",
        "content_gen=",
        "shadow_mem=",
        "shadow_gen=",
        "shadow_ok=",
        "import_external=",
        "import_present_generation=",
        "direct_import_ok=",
        "exported=",
        "shadow_exported=",
        "result=",
    ):
        if field not in export_text:
            fail(f"AV1 visible output check trace is missing field {field}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
