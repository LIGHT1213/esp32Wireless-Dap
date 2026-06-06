import pathlib
import sys

PATTERNS = {
    "success": ["Erase Done.", "Programming Done.", "Verify OK."],
    "cmsis_dap_missing": ["CMSIS-DAP not found", "No CMSIS-DAP"],
    "debug_entry": ["Cannot enter Debug Mode", "No Cortex-M Device found"],
    "swd_comm": ["SWD/JTAG Communication Failure", "No Debug Unit Device found"],
    "flash_download": ["Flash Download failed", "Error: Flash Download failed"],
    "dp_ap": ["Access Port", "Debug Port", "AP", "DP"],
}

def main() -> int:
    if len(sys.argv) != 2:
        print("usage: parse_keil_log.py <log-file>")
        return 2
    path = pathlib.Path(sys.argv[1])
    text = path.read_text(errors="ignore") if path.exists() else ""
    ok = all(token in text for token in PATTERNS["success"])
    print(f"success={ok}")
    for name, tokens in PATTERNS.items():
        if name == "success":
            continue
        hits = [token for token in tokens if token in text]
        if hits:
            print(f"{name}: " + ", ".join(hits))
    if not ok:
        print("next: check USB enumeration, ESP-NOW PING/PONG, DAP command logs, then SWD waveforms/reset.")
    return 0 if ok else 1

if __name__ == "__main__":
    raise SystemExit(main())
