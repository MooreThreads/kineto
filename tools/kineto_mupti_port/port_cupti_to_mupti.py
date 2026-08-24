#!/usr/bin/env python3
"""In-place whole-tree CUDA/CUPTI -> MUSA/MUPTI migration for Kineto.

The migration intentionally replaces the CUDA backend rather than adding a
second backend.  It has two passes:

1. longest-key-first mechanical text and path replacement;
2. MUPTI 5.1 compatibility fixups validated against the target SDK.

Use this only on a disposable branch or a recoverable copy of Kineto.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_RULES = HERE / "cupti_to_mupti.json"
TEXT_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".cmake", ".bzl", ".bazel", ".inc", ".in", ".txt", ".md",
    ".json", ".py", ".sh",
}
SKIP_DIRS = {".git", "build", "build_musa", "__pycache__"}
OLD_PATH_RE = re.compile(r"(?:^|/)(?:Cupti|cupti_strings|DeviceUtil|DeviceProperties)")

# Public profiler ABI names which the MUSA fork is expected to expose.  Unlike
# the old draft generator, no CUDA spelling is protected in the output tree.
EXTRA_RULES = (
    ("CUDA_SYNC", "MUSA_SYNC"),
    ("CUDA_EVENT", "MUSA_EVENT"),
    ("CUDA_PROFILER_RANGE", "MUSA_PROFILER_RANGE"),
    ("CUDA_OCC_SUCCESS", "MUSA_OCC_SUCCESS"),
    ("cudaOcc", "musaOcc"),
    ("CUDA_VERSION", "MUSA_VERSION"),
    ("CUDART_VERSION", "MUSART_VERSION"),
)

UNSUPPORTED_CALLS = (
    "muptiActivityEnableMusaEventDeviceTimestamps",
    "muptiActivityEnableRuntimeApi",
    "muptiActivityEnableDriverApi",
    "muptiActivityRegisterTimestampCallback",
)
UNSUPPORTED_ENUMS = (
    "MUPTI_ACTIVITY_ATTR_PER_THREAD_ACTIVITY_BUFFER",
    "MUPTI_ACTIVITY_OVERHEAD_RUNTIME_TRIGGERED_MODULE_LOADING",
    "MUPTI_ACTIVITY_OVERHEAD_LAZY_FUNCTION_LOADING",
    "MUPTI_ACTIVITY_OVERHEAD_COMMAND_BUFFER_FULL",
    "MUPTI_ACTIVITY_OVERHEAD_ACTIVITY_BUFFER_REQUEST",
)
FORBIDDEN_RE = re.compile(
    r"\b(?:HAS_)?(?:CUPTI|CUpti|Cupti|cupti)\w*"
    r"|\b(?:CUDA|cuda|Cuda)\w*"
)
ACTIVITY_TYPE_ABI_RE = re.compile(
    r"\bCUDA_(?:RUNTIME|DRIVER|SYNC|EVENT|PROFILER_RANGE)\b"
    r'|"cuda_(?:runtime|driver|sync|event|profiler_range)"'
)

@dataclass(frozen=True)
class FilePlan:
    source: Path
    destination: Path
    content: str
    renamed: bool


def load_rules(path: Path) -> list[tuple[str, str]]:
    pairs = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=list)
    rules = [(k, v) for k, v in pairs if not k.startswith("_")]
    rules.extend(EXTRA_RULES)
    return sorted(rules, key=lambda item: len(item[0]), reverse=True)


def find_kineto_root(src: Path) -> Path:
    src = src.resolve()
    candidates = [src, src / "libkineto"]
    if src.name == "src":
        candidates.insert(0, src.parent)
    for root in candidates:
        if (root / "src").is_dir() and (root / "CMakeLists.txt").is_file():
            return root
    raise SystemExit(f"{src} is not an official Kineto root, libkineto/, or libkineto/src/")


def iter_files(root: Path):
    for path in root.rglob("*"):
        if not path.is_file() or any(part in SKIP_DIRS for part in path.relative_to(root).parts):
            continue
        if path.suffix.lower() in TEXT_SUFFIXES or path.name in {"CMakeLists.txt", "BUILD"}:
            yield path


def rename_name(name: str) -> str:
    # Longest and most specific names first.
    for old, new in (
        ("cupti_strings", "mupti_strings"),
        ("Cupti", "Mupti"),
    ):
        name = name.replace(old, new)
    return name


def destination_for(path: Path, root: Path) -> Path:
    rel = path.relative_to(root)
    parts = [rename_name(part) for part in rel.parts]
    return root.joinpath(*parts)


def mechanical(text: str, rules: list[tuple[str, str]]) -> str:
    for old, new in rules:
        text = text.replace(old, new)
    return text


def replace_call_with_comment(text: str, function: str, reason: str) -> str:
    needle = f"MUPTI_CALL({function}("
    while (start := text.find(needle)) >= 0:
        line_start = text.rfind("\n", 0, start) + 1
        depth = 0
        end = None
        for pos in range(start, len(text)):
            if text[pos] == "(":
                depth += 1
            elif text[pos] == ")":
                depth -= 1
                if depth == 0:
                    end = pos + 1
                    while end < len(text) and text[end] in " \t":
                        end += 1
                    if end < len(text) and text[end] == ";":
                        end += 1
                    break
        if end is None:
            raise ValueError(f"unterminated {function} call")
        indent = text[line_start:start]
        text = text[:line_start] + indent + f"// Omitted for MUPTI 5.1: {reason}" + text[end:]
    return text


def compatibility(text: str) -> str:
    reasons = {
        "muptiActivityEnableMusaEventDeviceTimestamps": "event timestamp control is unavailable",
        "muptiActivityEnableRuntimeApi": "runtime is enabled by activity kind",
        "muptiActivityEnableDriverApi": "driver is enabled by activity kind",
        "muptiActivityRegisterTimestampCallback": "MUPTI 5.1 has no timestamp callback",
    }
    for function in UNSUPPORTED_CALLS:
        text = replace_call_with_comment(text, function, reasons[function])

    for enum in UNSUPPORTED_ENUMS:
        text = re.sub(
            rf"^[ \t]*case {re.escape(enum)}:\s*\n[ \t]*return [^;]+;\s*\n?",
            "",
            text,
            flags=re.MULTILINE,
        )

    # The target library declares but does not export this API.  Replace only
    # the two known setter bodies, preserving their public virtual interface.
    text = re.sub(
        r"void MuptiActivityApi::setDeviceBufferSize\([^)]*\)\s*\{.*?\n\}",
        "void MuptiActivityApi::setDeviceBufferSize([[maybe_unused]] size_t size) {\n  // Disabled: MUPTI 5.1 does not export muptiActivitySetAttribute.\n}",
        text,
        flags=re.DOTALL,
    )
    text = re.sub(
        r"void MuptiActivityApi::setDeviceBufferPoolLimit\([^)]*\)\s*\{.*?\n\}",
        "void MuptiActivityApi::setDeviceBufferPoolLimit([[maybe_unused]] size_t limit) {\n  // Disabled: MUPTI 5.1 does not export muptiActivitySetAttribute.\n}",
        text,
        flags=re.DOTALL,
    )

    # MUPTI 5.1 has no per-thread activity-buffer attribute.  The second
    # branch has SDK-version preprocessor guards, so remove its whole enclosing
    # if/else block rather than leaving an unavailable enum in dead code.
    text = re.sub(
        r"\n#if \(MUSART_VERSION >= 12030\)\n"
        r"  // Clear out per-thread buffer flag in case it was set\n.*?"
        r"#endif // \(MUSART_VERSION >= 12030\)\n",
        "\n",
        text,
        flags=re.DOTALL,
    )
    text = re.sub(
        r"\n  if \(enablePerThreadBuffers\) \{\n.*?"
        r"\n  \} else \{\n"
        r"    LOG\(VERBOSE\) << \(\"Not enabling per-thread activity buffer\"\);\n"
        r"  \}\n",
        "\n  (void)enablePerThreadBuffers; // Unsupported by MUPTI 5.1.\n",
        text,
        flags=re.DOTALL,
    )

    text = re.sub(
        r'([ \t]*)\{\"privateuse1_runtime\", ActivityType::PRIVATEUSE1_RUNTIME\},\n'
        r'[ \t]*\{\"privateuse1_driver\", ActivityType::PRIVATEUSE1_DRIVER\},',
        r'\1{"musa_runtime", ActivityType::PRIVATEUSE1_RUNTIME},\n'
        r'\1{"musa_driver", ActivityType::PRIVATEUSE1_DRIVER},',
        text,
    )

    # MUPTI 5.1 records omit fields introduced in newer CUPTI record versions.
    text = re.sub(
        r"\n#if MUSA_VERSION >= 12080\n"
        r"  return convertMuptiTimestamp\(activity_\.deviceTimestamp\);\n"
        r"#else\n"
        r"  // For MUSA < 12\.8, deviceTimestamp doesn't exist, set to 0\n"
        r"  return 0;\n"
        r"#endif",
        "\n  return 0;",
        text,
    )
    text = re.sub(
        r"\n  addPriorityTypedMetadata\(visitor, kernel\);\n"
        r"  addChannelTypedMetadata\(visitor, kernel\);",
        "",
        text,
    )
    text = re.sub(
        r"\n  addChannelTypedMetadata\(visitor, (?:memcpy|memset)\);",
        "",
        text,
    )
    text = text.replace('      {OCC_LIMIT_BARRIERS, "BARRIERS"},\n', "")
    text = re.sub(
        r"\n    d\.visit\(\n"
        r"        MusaMetadataFields::kBlockLimitBarriers,\n"
        r"        static_cast<int64_t>\(occMetrics\.result\.blockLimitBarriers\)\);",
        "",
        text,
    )

    text = text.replace("MUpti_ActivityMemcpy4Kind", "MUpti_ActivityMemcpyKind")
    text = re.sub(r"MUpti_ActivityKernel(?:[0-9]+)?\b", "MUpti_ActivityKernel6", text)
    text = re.sub(r"MUpti_ActivityMemcpyPtoP(?:[0-9]+)?\b", "MUpti_ActivityMemcpyPtoP3", text)
    text = re.sub(r"MUpti_ActivityMemcpy(?:[0-9]+)?\b", "MUpti_ActivityMemcpy4", text)
    text = re.sub(r"MUpti_ActivityMemset(?:[0-9]+)?\b", "MUpti_ActivityMemset3", text)
    text = re.sub(r"MUpti_Activity(?:Musa|Cuda)Event[0-9]*\b", "MUpti_ActivityMusaEvent", text)
    return text


def fix_timestamp(text: str, path: Path) -> str:
    if path.name == "MuptiTimestamp.h":
        text = re.sub(
            r"inline int64_t convertMuptiTimestamp\(uint64_t timestamp\)\s*\{.*?\n\}",
            "inline int64_t convertMuptiTimestamp(uint64_t timestamp) {\n"
            "  static const int64_t epochOffset = getTime() - getTime(true);\n"
            "  return timestamp + epochOffset;\n"
            "}",
            text,
            flags=re.DOTALL,
        )
        text = re.sub(
            r"inline int64_t convertMuptiDuration\(uint64_t start, uint64_t end\)\s*\{.*?\n\}",
            "inline int64_t convertMuptiDuration(uint64_t start, uint64_t end) {\n  return end - start;\n}",
            text,
            flags=re.DOTALL,
        )
    if path.name == "MuptiTimestamp.cpp" and "configureMuptiTimestampSource" in text:
        start = text.find("void configureMuptiTimestampSource(")
        end = text.find("\nbool isMuptiTimestampSourceReady()", start)
        if start >= 0 and end >= 0:
            text = text[:start] + (
                "void configureMuptiTimestampSource([[maybe_unused]] bool useTsc) {\n"
                "  // MUPTI 5.1 has no timestamp callback; use epoch conversion.\n"
                "  use_mupti_tsc() = false;\n"
                "  timestampSourceReady = true;\n}\n"
            ) + text[end:]
    return text


def fix_activity_type(text: str) -> str:
    """Preserve Kineto's serialized ABI while adding MUSA activity slots."""
    for migrated, stable in (
        (
            "  PRIVATEUSE1_RUNTIME = 7, // host side musa runtime events",
            "  CUDA_RUNTIME = 7, // host side cuda runtime events",
        ),
        (
            "  PRIVATEUSE1_DRIVER = 8, // host side musa driver events",
            "  CUDA_DRIVER = 8, // host side cuda driver events",
        ),
        (
            "  MUSA_SYNC = 15, // synchronization events between runtime and kernels",
            "  CUDA_SYNC = 15, // synchronization events between runtime and kernels",
        ),
        (
            "  MUSA_EVENT = 16, // MUSA event activities (musaEventRecord, etc.)",
            "  CUDA_EVENT = 16, // CUDA event activities (musaEventRecord, etc.)",
        ),
        ("  MUSA_PROFILER_RANGE = 19,", "  CUDA_PROFILER_RANGE = 19,"),
        (
            '{"musa_runtime", ActivityType::PRIVATEUSE1_RUNTIME}',
            '{"cuda_runtime", ActivityType::CUDA_RUNTIME}',
        ),
        (
            '{"musa_driver", ActivityType::PRIVATEUSE1_DRIVER}',
            '{"cuda_driver", ActivityType::CUDA_DRIVER}',
        ),
        (
            '{"musa_sync", ActivityType::MUSA_SYNC}',
            '{"cuda_sync", ActivityType::CUDA_SYNC}',
        ),
        (
            '{"musa_event", ActivityType::MUSA_EVENT}',
            '{"cuda_event", ActivityType::CUDA_EVENT}',
        ),
        (
            '{"musa_profiler_range", ActivityType::MUSA_PROFILER_RANGE}',
            '{"cuda_profiler_range", ActivityType::CUDA_PROFILER_RANGE}',
        ),
        (
            '{"privateuse1_runtime", ActivityType::PRIVATEUSE1_RUNTIME}',
            '{"musa_runtime", ActivityType::PRIVATEUSE1_RUNTIME}',
        ),
        (
            '{"privateuse1_driver", ActivityType::PRIVATEUSE1_DRIVER}',
            '{"musa_driver", ActivityType::PRIVATEUSE1_DRIVER}',
        ),
    ):
        text = text.replace(migrated, stable, 1)

    text = re.sub(
        r"\n  // PRIVATEUSE1 Activity types are used for custom backends\.\n"
        r"  // The corresponding device type is `DeviceType::PrivateUse1` in PyTorch\.\n",
        "\n",
        text,
    )
    text, enum_replacements = re.subn(
        r"(  HARDWARE_COUNTERS =\n"
        r"      28, // hardware performance counter samples\. Only used by MUSA right now\.\n)\n"
        r"  ENUM_COUNT =\n"
        r"      29, // This is to add buffer and not used for any profiling logic\. Add",
        r"\1  MUSA_PROFILER_RANGE = 29,\n"
        r"  MUSA_SYNC = 30, // synchronization events between runtime and kernels\n"
        r"  MUSA_EVENT = 31, // MUSA event activities (musaEventRecord, etc.)\n"
        r"  ENUM_COUNT = 32, // This is to add buffer and not used for any profiling logic. Add",
        text,
    )
    text, name_replacements = re.subn(
        r'(        \{"hardware_counters", ActivityType::HARDWARE_COUNTERS\},\n)'
        r'(        \{"ENUM_COUNT", ActivityType::ENUM_COUNT\},)',
        r'\1'
        r'        {"musa_profiler_range", ActivityType::MUSA_PROFILER_RANGE},\n'
        r'        {"musa_sync", ActivityType::MUSA_SYNC},\n'
        r'        {"musa_event", ActivityType::MUSA_EVENT},\n'
        r'\2',
        text,
    )
    if enum_replacements != 1 or name_replacements != 1:
        raise ValueError("ActivityType.h does not match the expected clean Kineto layout")
    return text


def apply_special_public_files(text: str, path: Path) -> str:
    if path.name == "ActivityType.h":
        text = fix_activity_type(text)
    if path.name == "ActivityBuffers.h":
        text = text.replace('#include "CuptiActivityBuffer.h"', '#include "MuptiActivityBuffer.h"')
        text = text.replace("CuptiActivityBufferMap", "MuptiActivityBufferMap")
    if path.name == "CMakeLists.txt":
        text = text.replace("KINETO_BACKEND STREQUAL \"cuda\"", "KINETO_BACKEND STREQUAL \"musa\"")
        text = text.replace("HAS_CUPTI", "HAS_MUPTI").replace("CUPTI", "MUPTI").replace("CUDA", "MUSA")
        # MUSA 5.1 does not ship a FindMUSAToolkit.cmake package. Discover the
        # headers and libraries directly from MUSA_SOURCE_DIR instead.
        toolkit_start = text.find("if(KINETO_BACKEND STREQUAL \"musa\")\n  # MUSA / MUPTI detection")
        toolkit_end = text.find("elseif(KINETO_BACKEND STREQUAL \"rocm\")", toolkit_start)
        if toolkit_start >= 0 and toolkit_end >= 0:
            text = text[:toolkit_start] + """if(KINETO_BACKEND STREQUAL \"musa\")
  set(MUSA_SOURCE_DIR \"$ENV{MUSA_HOME}\" CACHE PATH \"MUSA toolkit root\")
  if(NOT MUSA_SOURCE_DIR)
    set(MUSA_SOURCE_DIR \"/usr/local/musa\")
  endif()
  find_path(MUPTI_INCLUDE_DIR mupti.h
    PATHS ${MUSA_SOURCE_DIR}/include REQUIRED NO_DEFAULT_PATH)
  find_library(MUPTI_LIBRARY NAMES mupti libmupti.so.1
    HINTS ${MUSA_SOURCE_DIR}/lib REQUIRED NO_DEFAULT_PATH)
  find_library(MUSART_LIBRARY NAMES musart
    HINTS ${MUSA_SOURCE_DIR}/lib REQUIRED NO_DEFAULT_PATH)
  message(STATUS \"Found MUPTI: ${MUPTI_LIBRARY}\")
""" + text[toolkit_end:]
        text = text.replace(
            "  target_link_libraries(kineto PUBLIC MUSA::mupti MUSA::musart)\n"
            "  target_link_libraries(kineto_base PUBLIC MUSA::mupti MUSA::musart)",
            "  target_include_directories(kineto_base SYSTEM PUBLIC ${MUPTI_INCLUDE_DIR})\n"
            "  target_link_libraries(kineto PUBLIC ${MUPTI_LIBRARY} ${MUSART_LIBRARY})\n"
            "  target_link_libraries(kineto_base PUBLIC ${MUPTI_LIBRARY} ${MUSART_LIBRARY})",
        )
    return text


def build_plan(root: Path, rules: list[tuple[str, str]]) -> list[FilePlan]:
    plans = []
    destinations = {}
    for source in iter_files(root):
        destination = destination_for(source, root)
        text = source.read_text(encoding="utf-8", errors="strict")
        text = mechanical(text, rules)
        text = compatibility(text)
        text = fix_timestamp(text, destination)
        text = apply_special_public_files(text, destination)
        if destination in destinations and destinations[destination] != source:
            raise SystemExit(f"destination collision: {source} and {destinations[destination]} -> {destination}")
        destinations[destination] = source
        plans.append(FilePlan(source, destination, text, source != destination))
    return plans


def audit(root: Path, plans: list[FilePlan]) -> list[str]:
    findings = []
    for plan in plans:
        for lineno, line in enumerate(plan.content.splitlines(), 1):
            # Allow only the explicit MUSA ABI; the migration deliberately has
            # no CUDA/CUPTI spelling left in source or build configuration.
            # ActivityType.h intentionally retains only Kineto's serialized
            # CUDA ABI slots and names. Other CUDA/CUPTI leftovers still fail.
            if (
                plan.destination.name == "ActivityType.h"
                and ACTIVITY_TYPE_ABI_RE.search(line)
            ):
                continue
            if FORBIDDEN_RE.search(line):
                findings.append(f"{plan.destination.relative_to(root)}:{lineno}: {line.strip()[:160]}")
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", required=True, type=Path, help="Kineto root, libkineto, or libkineto/src")
    parser.add_argument("--rules", type=Path, default=DEFAULT_RULES)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    root = find_kineto_root(args.src)
    rules = load_rules(args.rules)
    plans = build_plan(root, rules)
    findings = audit(root, plans)
    renames = [p for p in plans if p.renamed]
    print(f"root: {root}")
    print(f"files: {len(plans)}, renames: {len(renames)}, audit findings: {len(findings)}")
    for plan in renames:
        print(f"  rename {plan.source.relative_to(root)} -> {plan.destination.relative_to(root)}")
    if findings:
        print("forbidden leftovers:")
        for finding in findings[:80]:
            print(f"  {finding}")
        if len(findings) > 80:
            print(f"  ... {len(findings) - 80} more")
        return 2
    if args.dry_run:
        print("dry-run: no files changed")
        return 0

    # Refuse ambiguous overwrites before changing anything.
    source_set = {p.source.resolve() for p in plans}
    for plan in plans:
        if plan.destination.exists() and plan.destination.resolve() not in source_set and plan.destination != plan.source:
            raise SystemExit(f"refusing to overwrite existing unrelated file: {plan.destination}")

    # Write transformed content first, then remove old names.  This is safe for
    # rename plans because every source was read and every destination checked.
    for plan in plans:
        plan.destination.parent.mkdir(parents=True, exist_ok=True)
        plan.destination.write_text(plan.content, encoding="utf-8")
    for plan in plans:
        if plan.source != plan.destination and plan.source.exists():
            plan.source.unlink()

    report = root / "MUSA_MUPTI_PORT_REPORT.md"
    report.write_text(
        "# In-place CUDA/CUPTI to MUSA/MUPTI migration\n\n"
        f"- root: `{root}`\n- transformed files: {len(plans)}\n"
        f"- renamed files: {len(renames)}\n- MUPTI 5.1 compatibility pass: applied\n"
        "- CUDA/CUPTI backend branches: replaced in place\n"
        "- old CUDA/CUPTI device paths: removed\n",
        encoding="utf-8",
    )
    print(f"migrated in place: {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
