import re
import os
import time
import subprocess

# --------------------------
# Configuration
# --------------------------
LOG_FILE = 'anomalies.log'
LINES_TO_CHECK = 50          # Process 50-line batches
CHECK_INTERVAL = 2           # seconds between polls

# Regex to parse new "Mismatch:" lines produced by AD
MISMATCH_REGEX = re.compile(
    r'Mismatch:\s*UE ID\s*([0-9.]+)\s*\|\s*time\s*([^\|]+)\|\s*Cluster=\d+\([^)]+\)\s*->\s*expected CellID\s*(\d+)\([^)]+\)\s*\|\s*Current CellID\s*(\d+)\([^)]+\)',
    re.IGNORECASE
)

# gRPC command template (unchanged)
GRPCURL_PATH = "grpcurl"
COMMAND_TEMPLATE = '''{grpcurl} -plaintext -d "{{ \\
    \\"e2NodeID\\": \\"36000000\\", \\
    \\"plmnID\\": \\"313131\\", \\
    \\"ranName\\": \\"gnb_131_133_31000000\\", \\
    \\"RICE2APHeaderData\\": {{ \\
        \\"RanFuncId\\": 300, \\
        \\"RICRequestorID\\": 1001 \\
    }}, \\
    \\"RICControlHeaderData\\": {{ \\
        \\"ControlStyle\\": 3, \\
        \\"ControlActionId\\": 1, \\
        \\"UEID\\": \\"{ueid}\\" \\
    }}, \\
    \\"RICControlMessageData\\": {{ \\
        \\"RICControlCellTypeVal\\": 4, \\
        \\"TargetCellID\\": \\"{target_cell}\\" \\
    }}, \\
    \\"RICControlAckReqVal\\": 0 \\
}}" 10.244.0.26:7777 rc.MsgComm.SendRICControlReqServiceGrpc'''

def compute_target_cell(cell_id: int) -> str:
    return f"1110{int(cell_id)}"

# --------------------------
# Run gRPC Command
# --------------------------
def run_grpc_command_for_slice(ueid: int, expected_cell_id: int):
    formatted_ueid = f"{ueid:05d}"
    target_cell = compute_target_cell(expected_cell_id)

    print(f"[INFO] Handover request → UEID={formatted_ueid} to TargetCellID={target_cell} (expected slice cell {expected_cell_id})")

    try:
        command = COMMAND_TEMPLATE.format(
            grpcurl=GRPCURL_PATH,
            ueid=formatted_ueid,
            target_cell=target_cell
        )
        subprocess.run(command, shell=True, check=True, executable="/bin/bash")
        print(f"[OK] gRPC sent for UEID {formatted_ueid} → {target_cell}")
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] gRPC failed for UEID {formatted_ueid}: {e}")
    finally:
        print("[INFO] Waiting 5 seconds before next command...")
        time.sleep(5)

# --------------------------
# Main Execution Loop
# --------------------------
def main():
    print("[INFO] Starting ts-final-tested.py (slice-based handover)...")
    current_position = 0  # Start from beginning of log file
    processed_ueids = set()  # Avoid duplicate handovers per UE

    while True:
        if not os.path.exists(LOG_FILE):
            print(f"[INFO] Log file '{LOG_FILE}' not found.")
            time.sleep(CHECK_INTERVAL)
            continue

        with open(LOG_FILE, 'r') as f:
            all_lines = f.readlines()

        total_lines = len(all_lines)
        remaining_lines = total_lines - current_position

        if remaining_lines < LINES_TO_CHECK:
            print(f"[INFO] Not enough new lines ({remaining_lines}) at position {current_position}. Waiting for new data...")
            time.sleep(CHECK_INTERVAL)
            continue

        batch_lines = all_lines[current_position:current_position + LINES_TO_CHECK]
        print(f"[DEBUG] Parsing {len(batch_lines)} lines from position {current_position}")

        # Parse only "Mismatch:" lines emitted by AD
        actions = []
        for line in batch_lines:
            m = MISMATCH_REGEX.search(line)
            if not m:
                continue

            ueid_raw, ts_str, expected_cell_str, current_cell_str = m.groups()

            # normalize UEID (handles "23.0" → 23)
            try:
                ueid = int(float(ueid_raw))
            except ValueError:
                print(f"[WARN] Could not parse UEID from line: {line.strip()}")
                continue

            try:
                expected_cell = int(expected_cell_str)
            except ValueError:
                print(f"[WARN] Could not parse expected CellID from line: {line.strip()}")
                continue

            # Deduplicate per-batch
            actions.append((ueid, expected_cell, ts_str.strip()))

        # Execute handovers for UEs we haven't processed yet
        for ueid, expected_cell, ts_str in actions:
            if ueid in processed_ueids:
                print(f"[INFO] Skipping UEID {ueid}: already processed.")
                continue

            print(f"[INFO] From log @ {ts_str}: UEID {ueid} → expected CellID {expected_cell}")
            run_grpc_command_for_slice(ueid, expected_cell)
            processed_ueids.add(ueid)

        current_position += LINES_TO_CHECK
        print(f"[INFO] Batch complete. Next batch starts at line {current_position}")
        time.sleep(CHECK_INTERVAL)

if __name__ == "__main__":
    main()