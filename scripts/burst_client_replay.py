import socket
import csv
import time
import sys
import threading

def send_request(model, batch, srv_ip="127.0.0.1", srv_port=8000):
    """ Connects, sends the r_model_batch request, and exits. """
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(5) # Don't hang if server is clogged
            s.connect((srv_ip, srv_port))
            # SwiftNNI Listener format: r_model_batch
            msg = f"r_{model}_{batch}"
            s.sendall(msg.encode())
            # We don't wait for the 'ACCEPTED' response here to maximize 
            # the burst pressure on the server's listener thread.
    except Exception:
        # In a high-load burst, some TCP connections might be refused.
        # We ignore them to keep the playback timing accurate.
        pass

def main():
    if len(sys.argv) < 4:
        print("Usage: python3 burst_client_replay.py <master_file> <lambda> <total_jobs>")
        sys.exit(1)

    master_file = sys.argv[1]
    lam = float(sys.argv[2])
    total_target = int(sys.argv[3])

    # 1. Load the Master Workload
    with open(master_file, 'r') as f:
        master_list = list(csv.DictReader(f))

    print(f"[Client] Replaying {total_target} jobs from {master_file} at Lambda={lam}")

    count = 0
    start_time = time.time()
    virtual_clock = 0.0

    # 2. Playback Loop
    while count < total_target:
        for j in master_list:
            if count >= total_target:
                break
            
            # Calculate when this job SHOULD arrive
            # The interval in the CSV is for Lambda=1.0
            wait_time = float(j['interval']) / lam
            virtual_clock += wait_time
            
            # 3. Synchronize with Real-world Time
            now = time.time() - start_time
            if virtual_clock > now:
                time.sleep(virtual_clock - now)
            
            # 4. Launch Request in a background thread
            # This ensures the client doesn't block the timing of the next job
            # while waiting for the TCP handshake.
            t = threading.Thread(target=send_request, args=(j['model'], j['batch']))
            t.daemon = True
            t.start()
            
            count += 1
            if count % 100 == 0:
                print(f"  ... Sent {count}/{total_target} jobs")

    # Give the last few threads a second to finish their handshakes
    time.sleep(1)
    print(f"[Client] Replay complete. Total time: {round(time.time() - start_time, 2)}s")

if __name__ == "__main__":
    main()