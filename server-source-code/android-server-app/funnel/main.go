package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"tailscale.com/tsnet"
)

var outputLock sync.Mutex

// report sends one status update to the Android service.
func report(kind string, value string) {
	outputLock.Lock()
	defer outputLock.Unlock()
	json.NewEncoder(os.Stdout).Encode(map[string]string{"type": kind, "value": value})
}

// run owns both public listeners and closes them when hosting stops.
func run(ctx context.Context, state string, httpPort int, websocketPort int) error {
	if httpPort < 1024 || httpPort > 65535 || websocketPort < 1024 || websocketPort > 65535 || httpPort == websocketPort {
		return errors.New("choose different HTTP and WebSocket ports from 1024 to 65535")
	}
	if state == "" || !filepath.IsAbs(state) {
		return errors.New("a private absolute state directory is required")
	}
	if err := os.MkdirAll(state, 0700); err != nil {
		return err
	}
	if err := waitForLocalServers(ctx, httpPort, websocketPort); err != nil {
		return err
	}
	server := &tsnet.Server{
		Dir:      state,
		Hostname: "lemon-chat",
		Logf: func(string, ...any) {
		},
		UserLogf: func(format string, arguments ...any) {
			message := fmt.Sprintf(format, arguments...)
			for _, word := range strings.Fields(message) {
				if strings.HasPrefix(word, "https://login.tailscale.com/") {
					report("login", word)
				}
			}
		},
	}
	defer server.Close()
	report("status", "Sign in to Tailscale if requested.")
	status, err := server.Up(ctx)
	if err != nil {
		return err
	}
	if len(status.CertDomains) == 0 {
		return errors.New("enable HTTPS certificates for this device in the Tailscale admin console")
	}
	localClient, err := server.LocalClient()
	if err != nil {
		return err
	}
	certificateContext, cancelCertificate := context.WithTimeout(ctx, 2*time.Minute)
	defer cancelCertificate()
	report("status", "Preparing the HTTPS certificate.")
	if _, _, err := localClient.CertPair(certificateContext, status.CertDomains[0]); err != nil {
		return fmt.Errorf("HTTPS certificate: %w", err)
	}
	pageListener, err := server.ListenFunnel("tcp", ":443")
	if err != nil {
		return fmt.Errorf("HTTPS Funnel: %w", err)
	}
	defer pageListener.Close()
	socketListener, err := server.ListenFunnel("tcp", ":8443")
	if err != nil {
		return fmt.Errorf("WSS Funnel: %w", err)
	}
	defer socketListener.Close()
	pageServer := &http.Server{Handler: proxy(httpPort), ReadHeaderTimeout: 10 * time.Second, IdleTimeout: 60 * time.Second}
	socketServer := &http.Server{Handler: proxy(websocketPort), ReadHeaderTimeout: 10 * time.Second, IdleTimeout: 60 * time.Second}
	defer pageServer.Close()
	defer socketServer.Close()
	failures := make(chan error, 2)
	go func() {
		failures <- pageServer.Serve(pageListener)
	}()
	go func() {
		failures <- socketServer.Serve(socketListener)
	}()
	report("ready", "https://"+status.CertDomains[0]+"/")
	select {
	case <-ctx.Done():
		return nil
	case err := <-failures:
		return err
	}
}

func main() {
	state := flag.String("state", "", "Private Tailscale state directory")
	httpPort := flag.Int("http-port", 8080, "Local HTTP port")
	websocketPort := flag.Int("websocket-port", 1111, "Local WebSocket port")
	flag.Parse()
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	go func() {
		io.Copy(io.Discard, os.Stdin)
		cancel()
	}()
	if err := run(ctx, *state, *httpPort, *websocketPort); err != nil && ctx.Err() == nil {
		report("error", err.Error())
		os.Exit(1)
	}
}
