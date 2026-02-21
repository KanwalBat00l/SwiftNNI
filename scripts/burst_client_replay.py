import subprocess
import csv
import time
import sys
import threading
import os

def launch_real_client(model, batch, srv_ip, srv_port, job_id):
    """ Executes the actual C++ Swift_client binary """
    try:
        cmd = ["./Swift_client", srv_ip, str(srv_port), model, str(batch)]
        
        # Run and capture result
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        
        if result.returncode == 0:
            print(f"  [Client {job_id}] SUCCESS: {model}_{batch}")
        else:
            print(f"  [Client {job_id}] FAILED: {model}_{batch} (RC: {result.returncode})")
            
    except Exception as e:
        print(f"  [Client {job_id}] EXCEPTION: {e}")

def main():
    if len(sys.argv) < 4:
        print("Usage: python3 burst_client_replay.py <master_file> <lambda> <total_jobs> [srv_ip] [srv_port]")
        sys.exit(1)

    master_file = sys.argv[1]
    lam = float(sys.argv[2])
    total_target = int(sys.argv[3])
    srv_ip = sys.argv[4] if len(sys.argv) > 4 else "127.0.0.1"
    srv_port = int(sys.argv[5]) if len(sys.argv) > 5 else 8000

    if not os.path.exists(master_file):
        print(f"Error: {master_file} not found.")
        sys.exit(1)

    with open(master_file, 'r') as f:
        master_list = list(csv.DictReader(f))

    print(f"[Launcher] Starting {total_target} jobs | Lambda: {lam}")

    count = 0
    start_time = time.time()
    virtual_clock = 0.0
    threads = []

    # 1. Playback Loop
    while count < total_target:
        for j in master_list:
            if count >= total_target:
                break
            
            wait_time = float(j['interval']) / lam
            virtual_clock += wait_time
            
            now = time.time() - start_time
            if virtual_clock > now:
                time.sleep(virtual_clock - now)
            
            # 2. Launch Client (NOT as daemon)
            t = threading.Thread(target=launch_real_client, 
                                 args=(j['model'], j['batch'], srv_ip, srv_port, count+1))
            # Removing daemon=True ensures the thread stays alive until the C++ process finishes
            t.start()
            threads.append(t)
            
            count += 1
            if count % 20 == 0:
                print(f"[Status] Launched {count}/{total_target} jobs...")

    print(f"[Launcher] All {total_target} requests sent. Waiting for all inferences to complete...")
    
    # 3. Wait for every single thread to finish
    for i, t in enumerate(threads):
        t.join()
        if (i+1) % 20 == 0:
            print(f"  ... {i+1} inferences joined.")

    print("[Launcher] All jobs completed. You can now stop the server.")

if __name__ == "__main__":
    main()