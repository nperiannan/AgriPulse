package main

import (
	"encoding/json"
	"log"
	"net/http"
	"strings"
	"sync"
	"time"
)

// ---------------------------------------------------------------------------
// MQTT round-trip heartbeat
//
// Passive monitoring (status messages, mosquitto connect/disconnect logs) only
// proves "device → broker" is alive. It does NOT prove that a command sent
// backend → broker → device → broker → backend actually completes — which is
// exactly the class of failure that caused the 2026-07-19 incident (the
// backend's own MQTT client was silently disconnected for ~11 min while a
// motor-ON command was sent).
//
// Flow (initiated by the controller, per the requested design):
//  1. Controller periodically publishes a request on tm/{mac}/hb:
//       {"mac":"...", "seq": N}
//  2. Backend (onHeartbeatMsg) immediately replies with a normal control
//     command: {"cmd":"ping","seq": N} on tm/{mac}/control.
//  3. Controller's MQTT command handler acks it straight back on tm/{mac}/hb:
//       {"mac":"...", "ack_seq": N}
//  4. Backend records the round-trip latency and marks the device "healthy".
//     If no ack arrives within heartbeatAckTimeout, checkHeartbeatTimeouts()
//     (run on a ticker) logs a warning — this is the signal that would have
//     caught the 2026-07-19 outage in real time instead of after the fact.
// ---------------------------------------------------------------------------

const (
	heartbeatAckTimeout  = 20 * time.Second
	heartbeatCheckPeriod = 15 * time.Second
)

type heartbeatState struct {
	lastReqSeq        uint32
	lastReqAt         time.Time
	lastAckAt         time.Time
	lastLatency       time.Duration
	healthy           bool
	consecutiveMisses int
}

var (
	heartbeatMu    sync.Mutex
	heartbeatCache = make(map[string]*heartbeatState)
)

// onHeartbeatMsg handles messages on tm/{mac}/hb — both the controller's
// periodic request ("seq") and its ack of a prior ping ("ack_seq").
func onHeartbeatMsg(topic string, raw []byte) {
	mac := macFromTopic(topic)
	if mac == "" {
		return
	}
	var msg struct {
		Seq    uint32 `json:"seq"`
		AckSeq uint32 `json:"ack_seq"`
	}
	if err := json.Unmarshal(raw, &msg); err != nil {
		log.Printf("[HB] bad JSON on %s: %v", topic, err)
		return
	}

	heartbeatMu.Lock()
	st, ok := heartbeatCache[mac]
	if !ok {
		st = &heartbeatState{}
		heartbeatCache[mac] = st
	}

	if msg.AckSeq != 0 {
		// Ack for a previously sent ping.
		if msg.AckSeq == st.lastReqSeq && !st.lastReqAt.IsZero() {
			st.lastAckAt = time.Now()
			st.lastLatency = st.lastAckAt.Sub(st.lastReqAt)
			st.healthy = true
			st.consecutiveMisses = 0
			log.Printf("[HB] %s round-trip OK seq=%d latency=%s", mac, msg.AckSeq, st.lastLatency.Round(time.Millisecond))
		}
		heartbeatMu.Unlock()
		return
	}
	heartbeatMu.Unlock()

	if msg.Seq == 0 {
		return
	}

	// Heartbeat REQUEST from the controller — reply with a "ping" command
	// over the normal control channel (proves the full round trip, not just
	// that this /hb topic is reachable).
	heartbeatMu.Lock()
	st.lastReqSeq = msg.Seq
	st.lastReqAt = time.Now()
	heartbeatMu.Unlock()

	body, _ := json.Marshal(map[string]interface{}{"cmd": "ping", "seq": msg.Seq})
	if err := publishControl(mac, body); err != nil {
		log.Printf("[HB] %s failed to publish ping reply: %v", mac, err)
	}
}

// startHeartbeatMonitor runs a ticker that flags devices whose heartbeat
// request has gone unanswered for longer than heartbeatAckTimeout.
func startHeartbeatMonitor() {
	ticker := time.NewTicker(heartbeatCheckPeriod)
	defer ticker.Stop()
	for range ticker.C {
		checkHeartbeatTimeouts()
	}
}

func checkHeartbeatTimeouts() {
	heartbeatMu.Lock()
	defer heartbeatMu.Unlock()
	now := time.Now()
	for mac, st := range heartbeatCache {
		if st.lastReqAt.IsZero() || st.lastAckAt.After(st.lastReqAt) {
			continue // no pending request, or it was already acked
		}
		if now.Sub(st.lastReqAt) > heartbeatAckTimeout {
			st.healthy = false
			st.consecutiveMisses++
			log.Printf("[HB] %s heartbeat ACK timeout (seq=%d, waited %s) — possible MQTT command-path issue, consecutive misses=%d",
				mac, st.lastReqSeq, now.Sub(st.lastReqAt).Round(time.Second), st.consecutiveMisses)
			st.lastReqAt = now // avoid re-logging every tick until the next request
		}
	}
}

type heartbeatStatus struct {
	Healthy           bool   `json:"healthy"`
	LastReqAt         string `json:"last_req_at,omitempty"`
	LastAckAt         string `json:"last_ack_at,omitempty"`
	LatencyMs         int64  `json:"latency_ms,omitempty"`
	ConsecutiveMisses int    `json:"consecutive_misses"`
}

// handleDeviceHeartbeat serves GET /api/devices/{mac}/heartbeat — lets the
// app/web UI show whether the command channel to a device has been recently
// verified end-to-end (distinct from the passive "online" flag).
func handleDeviceHeartbeat(w http.ResponseWriter, r *http.Request) {
	cors(w)
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	mac := strings.ToUpper(macFromPath(r.URL.Path, "/api/devices/"))

	heartbeatMu.Lock()
	st, ok := heartbeatCache[mac]
	var out heartbeatStatus
	if ok {
		out.Healthy = st.healthy
		out.ConsecutiveMisses = st.consecutiveMisses
		if !st.lastReqAt.IsZero() {
			out.LastReqAt = st.lastReqAt.Format(time.RFC3339)
		}
		if !st.lastAckAt.IsZero() {
			out.LastAckAt = st.lastAckAt.Format(time.RFC3339)
			out.LatencyMs = st.lastLatency.Milliseconds()
		}
	}
	heartbeatMu.Unlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(out) //nolint:errcheck
}
