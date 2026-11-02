package main

import (
	"context"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"time"
)

// waitForLocalServers checks both destinations before opening public listeners.
func waitForLocalServers(ctx context.Context, httpPort int, websocketPort int) error {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	transport := &http.Transport{Proxy: nil}
	defer transport.CloseIdleConnections()
	client := &http.Client{Timeout: time.Second, Transport: transport}
	ticker := time.NewTicker(200 * time.Millisecond)
	defer ticker.Stop()
	for {
		request, err := http.NewRequestWithContext(ctx, "GET", fmt.Sprintf("http://127.0.0.1:%d/client.html", httpPort), nil)
		if err != nil {
			return err
		}
		response, err := client.Do(request)
		if err == nil {
			response.Body.Close()
			if response.StatusCode == http.StatusOK {
				dialer := net.Dialer{Timeout: time.Second}
				connection, err := dialer.DialContext(ctx, "tcp", fmt.Sprintf("127.0.0.1:%d", websocketPort))
				if err == nil {
					connection.Close()
					return nil
				}
			}
		}
		select {
		case <-ctx.Done():
			return fmt.Errorf("the local web page or chat server is unavailable: %w", ctx.Err())
		case <-ticker.C:
		}
	}
}

// proxy forwards HTTP and WebSocket upgrades to one local server port.
func proxy(port int) http.Handler {
	target := &url.URL{Scheme: "http", Host: fmt.Sprintf("127.0.0.1:%d", port)}
	return &httputil.ReverseProxy{
		Rewrite: func(request *httputil.ProxyRequest) {
			request.SetURL(target)
			request.Out.Host = request.In.Host
			request.SetXForwarded()
		},
		FlushInterval: -1,
		ErrorLog:      log.New(io.Discard, "", 0),
		ErrorHandler: func(response http.ResponseWriter, request *http.Request, err error) {
			http.Error(response, "The chat server is unavailable.", http.StatusBadGateway)
		},
	}
}
