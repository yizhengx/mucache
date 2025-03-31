package main

import (
	"context"
	"fmt"
	"github.com/eniac/mucache/internal/trivial"
	"github.com/eniac/mucache/pkg/slowpoke"
	"github.com/eniac/mucache/pkg/wrappers"
	"net"
	"net/http"
	"sync"
)

var lock sync.Mutex

func ep1(ctx context.Context, request *trivial.TrivialRequest) *trivial.TrivialResponse {
	slowpoke.SlowpokeCheck("ep1")
	//lock.Lock()
	//defer lock.Unlock()
	slowpoke.CPUSpinTime(350)
	resp := trivial.TrivialResponse{A: "ok"}
	return &resp
}

func main() {
	http.HandleFunc("/ep1", wrappers.NonROWrapper[trivial.TrivialRequest, trivial.TrivialResponse](ep1))
	slowpoke.SlowpokeInit()
	fmt.Println("Server started on :3000")
	listener, err := net.Listen("tcp", ":3000")
	if err != nil {
		panic(err)
	}
	slowpokeListener := &slowpoke.SlowpokeListener{listener}
	panic(http.Serve(slowpokeListener, nil))
}
