package slowpoke

import (
	"golang.org/x/sys/unix"
	"encoding/json"
	"fmt"
	"net/http"
	"bytes"
	"os"
	"time"
	"context"
	"runtime"
	"github.com/eniac/mucache/pkg/common"
	"github.com/eniac/mucache/pkg/utility"
)

const printIntervalMillis = 30*1000
// {"thread_id" :{"requestName": counter}}
var (
	requestCounters map[int]map[string]int
	delayMicros int
)

func printCounters() {
	time_now := time.Now()
	func_counters := make(map[string]int)
	string_to_print := ""
	for thread, counters := range requestCounters {
		for requestName, count := range counters {
			if count > 0 {
				if _, ok := func_counters[requestName]; !ok {
					func_counters[requestName] = 0
				}
				func_counters[requestName] += count
				string_to_print += fmt.Sprintf("	[%d] %s: %d\n", thread, requestName, count)
				requestCounters[thread][requestName] = 0
			}
		}
	}
	if string_to_print != "" {
		fmt.Printf("[%s] Slowpoke Counters\n", time_now.String())
		fmt.Printf(string_to_print)
		fmt.Printf("	[Aggregation]\n")
		for func_name, count := range func_counters {
			fmt.Printf("		%s: %d\n", func_name, count)
		}
	}
}


// Saves the response to *res (also might save the result to cache if we are in upperbound baseline
func performRequest[T interface{}](ctx context.Context, req *http.Request, res *T, app string, method string, argBytes []byte) {
	resp, err := common.HTTPClient.Do(req)
	if err != nil {
		panic(err)
	}
	utility.Assert(resp.StatusCode == http.StatusOK)
	defer resp.Body.Close()
	utility.ParseJson(resp.Body, res)
}

func SlowpokeInit() {
	delayMicros = -1
	if env, ok := os.LookupEnv("SLOWPOKE_DELAY_MICROS"); ok {
		fmt.Sscanf(env, "%d", &delayMicros)
		fmt.Printf("SLOWPOKE_DELAY_MICROS=%d\n", delayMicros)
	}
	requestCounters = make(map[int]map[string]int)
	// go func() {
	// 	for {
	// 		<-time.After(printIntervalMillis * time.Millisecond)
	// 		printCounters()
	// 	}
	// }()
}

// Get the amount of time in nanoseconds the calling thread has spent using the CPU since startup
func getThreadCPUTime() int64 {
	time := unix.Timespec{}
	unix.ClockGettime(unix.CLOCK_THREAD_CPUTIME_ID, &time)
	return time.Nano()
}


func SlowpokeCheck(serviceFuncName string) {
	// // Record request
	// if _, ok := requestCounters[unix.Gettid()]; !ok {
	// 	requestCounters[unix.Gettid()] = make(map[string]int)
	// }
	// if _, ok := requestCounters[unix.Gettid()][serviceFuncName]; !ok {
	// 	requestCounters[unix.Gettid()][serviceFuncName] = 0
	// }
	// requestCounters[unix.Gettid()][serviceFuncName]++
	// Delay
	lockThread := true
	if delayMicros >= 0 {
		// Threads need to be locked because otherwise util.ThreadCPUTime() can change in the middle of execution
		if lockThread {
			runtime.LockOSThread()
		}

		start := getThreadCPUTime()
		target := start + int64(delayMicros*1000.0)

		for getThreadCPUTime() < target {
		}

		if lockThread {
			runtime.UnlockOSThread()
		}
	}
}

func Invoke[T interface{}](ctx context.Context, app string, method string, input interface{}) T {
	buf, err := json.Marshal(input)
	if err != nil {
		panic(err)
	}
	var res T
	// Use kubernete native DNS addr
	url := fmt.Sprintf("http://%s.%s.svc.cluster.local:%s/%s", app, "default", "80", method)
	req, err := http.NewRequest("POST", url, bytes.NewBuffer(buf))
	if err != nil {
		panic(err)
	}
	performRequest[T](ctx, req, &res, app, method, buf)
	return res
}