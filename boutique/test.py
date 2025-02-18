#!python3

import os
import subprocess

TARGET_SERVICE = "cart"
TARGET_PROCESSING_TIME_RANGE = [0, 3000]

def exp(service_delay, request="home"):
    env = os.environ.copy()
    for service, delay in service_delay.items():
        env[f"SLOWPOKE_DELAY_MICROS_{service.upper()}"] = str(delay)
    cmd = f"bash run.sh {request}"
    print(f"[test.py] Running {request} request with the following configuration: {service_delay}", flush=True)
    process = subprocess.Popen(cmd, shell=True, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    print(f"[test.py] Executing {cmd}:")
    throughput = -1
    for line in process.stdout:
        line_output = line.decode().strip()
        if "Requests/sec" in line_output:
            throughput = float(line_output.split()[1])
        print(f"    {line.decode().strip()}", flush=True)
    if process.wait() != 0:
        print(f"Error running {cmd}")
        for line in process.stderr:
            print(f"    {line.decode().strip()}", flush=True)
        raise Exception(f"Error running {cmd}")
    return throughput

def run():
    print("[test.py] Start of experiment")
    service_delay = {
        "cart":0,
        "checkout":0,
        "currency":0,
        "email":0,
        "frontend":0,
        "payment":0,
        "product_catalog":0,
        "recommendations":0,
        "shipping":0
    }
    processing_time_diff = TARGET_PROCESSING_TIME_RANGE[1]-TARGET_PROCESSING_TIME_RANGE[0]
    processing_time_range = range(TARGET_PROCESSING_TIME_RANGE[0], TARGET_PROCESSING_TIME_RANGE[1], processing_time_diff//10)

    # groundtruth
    groundtruth = []
    for p_t in processing_time_range:
        service_delay[TARGET_SERVICE] = p_t
        groundtruth.append(exp(service_delay))
    
    # slowdown
    slowdown = []
    predicted = []
    for p_t in processing_time_range:
        for service in service_delay:
            if service == TARGET_SERVICE:
                service_delay[service] = TARGET_PROCESSING_TIME_RANGE[1]
            else:
                service_delay[service] = TARGET_PROCESSING_TIME_RANGE[1] - p_t
        slowdown.append(exp(service_delay))
        predicted_throughput = 1000000/(1000000/slowdown[-1]-p_t)
        predicted.append(predicted_throughput)
    
    err = [predicted[i]-groundtruth[i] for i in range(len(predicted))]
    print("[test.py] Groundtruth: ", groundtruth, flush=True)
    print("[test.py] Slowdown: ", slowdown, flush=True)
    print("[test.py] Predicted: ", predicted, flush=True)
    print("[test.py] Error ", err, flush=True)

    return groundtruth, slowdown, predicted, err

os.chdir(os.path.dirname(os.path.abspath(__file__)))
groundtruths, slowdowns, predicteds, errs = [], [], [], []
for i in range(1):
    groundtruth, slowdown = run()
    groundtruths.append(groundtruth)
    slowdowns.append(slowdown)
    predicteds.append(predict)
    errs.append(err)
print("[test.py] Summary: ")
for i in range(len(groundtruths[0])):
    print(f"[test.py] Result for the experiment {i}: ")
    print(f"    Groundtruth: {groundtruths[i]}")
    print(f"    Slowdown:    {slowdowns[i]}")
    print(f"    Predicted:   {predicteds[i]}")
    print(f"    Error:       {errs[i]}")