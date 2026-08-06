package main

import (
	"encoding/json"
	"fmt"
	"log"
	"strings"
	"sync"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

var mqttCli mqtt.Client

// deviceStatusMu guards per-device last-status cache.
var deviceStatusMu sync.RWMutex
var deviceStatus = make(map[string][]byte) // mac → raw JSON

// allowedCmds is the whitelist for inbound control requests.
var allowedCmds = map[string]bool{
	"oh_on": true, "oh_off": true,
	"ug_on": true, "ug_off": true,
	"sched_add": true, "sched_remove": true, "sched_clear": true,
	"set_setting": true, "sync_ntp": true, "reboot": true,
	"set_lcd_mode": true, "set_log_level": true, "get_logs": true,
	"sync":           true, // manual "pull to refresh" — forces an immediate status publish
	"set_mqtt_creds": true,
	"ota_start":      true, "ota_rollback": true,
	"wifi_list": true, "wifi_scan": true,
	"wifi_add": true, "wifi_delete": true, "wifi_set_priority": true,
	"history_list": true, "history_clear": true,
	"ping": true, // heartbeat round-trip check (see heartbeat.go)
}

// mqttLostAt records when the platform's own MQTT connection dropped, so the
// reconnect log can report the outage duration (helps diagnose incidents like
// 2026-07-19 where a slow reconnect silently dropped a motor command).
var mqttLostAt time.Time

// pubStore is an explicit handle to the client's QoS1 outbound message store.
// paho normally creates one of these internally and hides it — we supply our
// own MemoryStore via SetStore() below purely so publishControl() can purge a
// specific un-acked message (see dropPendingPublish). Without this, a QoS1
// publish that doesn't get PUBACK'd before we give up would otherwise sit in
// paho's store and get silently RESENT whenever the client next reconnects —
// even minutes later — which is exactly the kind of stale/late motor command
// this timeout is meant to prevent.
var pubStore = mqtt.NewMemoryStore()

// commandTimeout returns how long publishControl() should wait for the
// broker to PUBACK a command before giving up and dropping it. Motor ON
// commands get a short leash (better to fail fast and let the user retry than
// have a stale ON fire late); OFF commands get a bit longer since a delayed
// OFF is safer than a delayed ON but still shouldn't hang around indefinitely.
func commandTimeout(cmd string) time.Duration {
	switch cmd {
	case "oh_on", "ug_on":
		return 5 * time.Second
	case "oh_off", "ug_off":
		return 10 * time.Second
	default:
		return 10 * time.Second
	}
}

// dropPendingPublish removes a QoS1 publish from the store so paho won't
// resend it after a future reconnect. Replicates paho.mqtt.golang v1.4.3's
// internal outbound key format (store.go: outboundKeyFromMID → "o.<mid>") —
// pinned go.mod version, so this is stable; re-verify if the paho version is
// ever bumped.
func dropPendingPublish(mid uint16) {
	pubStore.Del(fmt.Sprintf("o.%d", mid))
}

func startMQTT() {
	broker := env("MQTT_BROKER", "localhost")
	port := env("MQTT_PORT", "1883")
	user := env("MQTT_USER", "tankmonitor")
	pass := env("MQTT_PASS", "###TankMonitor12345")

	opts := mqtt.NewClientOptions().
		AddBroker(fmt.Sprintf("tcp://%s:%s", broker, port)).
		SetClientID("tankmonitor-platform").
		SetUsername(user).
		SetPassword(pass).
		SetKeepAlive(60 * time.Second).
		SetAutoReconnect(true).
		// Paho defaults to a 10-MINUTE max backoff between reconnect attempts.
		// On 2026-07-19 that caused an ~11 min gap where this backend's own MQTT
		// client was disconnected from the broker (root cause of a dropped motor
		// ON command) — cap it much lower so we recover within seconds.
		SetMaxReconnectInterval(10 * time.Second).
		SetStore(pubStore).
		SetOnConnectHandler(func(c mqtt.Client) {
			// Subscribe to ALL device namespaces using + wildcard
			// New topic scheme: tm/+/status, tm/+/logs, tm/+/hb (heartbeat)
			// Legacy topic scheme: tankmonitor/+/status, tankmonitor/+/logs (backward compat)
			subs := map[string]byte{
				"tm/+/status":          1,
				"tm/+/logs":            0,
				"tm/+/wifi":            0,
				"tm/+/hb":              1,
				"tankmonitor/+/status": 1,
				"tankmonitor/+/logs":   0,
			}
			if !mqttLostAt.IsZero() {
				log.Printf("[MQTT] Connected — subscribing to %d wildcard topics (was disconnected for %s)",
					len(subs), time.Since(mqttLostAt).Round(time.Second))
				mqttLostAt = time.Time{}
			} else {
				log.Printf("[MQTT] Connected — subscribing to %d wildcard topics", len(subs))
			}
			c.SubscribeMultiple(subs, onMessage)
		}).
		SetConnectionLostHandler(func(_ mqtt.Client, err error) {
			mqttLostAt = time.Now()
			log.Printf("[MQTT] Connection lost: %v", err)
		})

	mqttCli = mqtt.NewClient(opts)
	for {
		if tok := mqttCli.Connect(); tok.Wait() && tok.Error() == nil {
			break
		}
		log.Println("[MQTT] Connect failed — retrying in 5s…")
		time.Sleep(5 * time.Second)
	}

	go startHeartbeatMonitor()
}

// onMessage handles all incoming MQTT messages from all subscribed topics.
func onMessage(_ mqtt.Client, msg mqtt.Message) {
	topic := msg.Topic()
	raw := make([]byte, len(msg.Payload()))
	copy(raw, msg.Payload())

	if strings.HasSuffix(topic, "/status") {
		onStatusMsg(topic, raw)
	} else if strings.HasSuffix(topic, "/logs") {
		onLogsMsg(topic, raw)
	} else if strings.HasSuffix(topic, "/wifi") {
		onWifiMsg(topic, raw)
	} else if strings.HasSuffix(topic, "/hb") {
		onHeartbeatMsg(topic, raw)
	}
}

func onStatusMsg(topic string, raw []byte) {
	// Parse the minimal fields we need for platform routing
	var fields struct {
		MAC        string `json:"mac"`
		DeviceType string `json:"device_type"`
		FW         string `json:"fw"`
		// OTA detection fields
		// (OTA is now per-device in ota.go)
	}
	if err := json.Unmarshal(raw, &fields); err != nil {
		log.Printf("[MQTT] bad JSON on %s: %v", topic, err)
		return
	}

	mac := fields.MAC
	if mac == "" {
		// Legacy firmware without mac field — derive mac from topic location
		// Topic: tankmonitor/{location}/status — we don't have a real MAC, use location as key
		mac = macFromTopic(topic)
	}
	if mac == "" {
		return
	}

	// Auto-register/update the device in the DB
	upsertDevice(mac, fields.DeviceType, fields.FW)

	// Cache latest status per device (capture previous for edge detection)
	deviceStatusMu.Lock()
	prev := deviceStatus[mac]
	deviceStatus[mac] = raw
	deviceStatusMu.Unlock()

	// Push notifications on motor on/off and transmitter-lost transitions
	detectAndPushEdges(mac, prev, raw)

	// Detect OTA success for this device
	otaOnStatusReceived(mac, fields.FW)

	// Fan out to per-device WebSocket subscribers
	wsHub.broadcast(mac, raw)
}

func onLogsMsg(topic string, raw []byte) {
	mac := macFromTopic(topic)
	if mac == "" {
		return
	}
	logsStore(mac, raw)
}

// publishControl publishes a control command to the correct device topic.
// Waits at most commandTimeout(cmd) for the broker to PUBACK; on timeout the
// message is purged from the store (dropPendingPublish) instead of being left
// for paho to resend on a later reconnect, and an error is returned so the
// caller/app sees "not delivered" rather than a silent late action.
func publishControl(mac string, body []byte) error {
	if mqttCli == nil || !mqttCli.IsConnected() {
		return fmt.Errorf("MQTT not connected")
	}
	// Track remote motor commands so derived history can infer a "Manual" reason.
	noteRemoteMotorCmd(mac, body)
	// Determine which topic scheme the device uses by checking if it has a real MAC
	// New firmware uses tm/{mac}/control; legacy uses tankmonitor/{location}/control
	var topic string
	if isRealMAC(mac) {
		topic = fmt.Sprintf("tm/%s/control", mac)
	} else {
		// Legacy: mac field holds the location string
		topic = fmt.Sprintf("tankmonitor/%s/control", mac)
	}

	var cmdFields struct {
		Cmd string `json:"cmd"`
	}
	json.Unmarshal(body, &cmdFields) //nolint:errcheck
	timeout := commandTimeout(cmdFields.Cmd)

	tok := mqttCli.Publish(topic, 1, false, body)
	if !tok.WaitTimeout(timeout) {
		if pt, ok := tok.(*mqtt.PublishToken); ok {
			dropPendingPublish(pt.MessageID())
		}
		log.Printf("[MQTT] %s command to %s not acked by broker within %s — dropped, not queued for later delivery",
			cmdFields.Cmd, mac, timeout)
		return fmt.Errorf("command not delivered within %s", timeout)
	}
	return tok.Error()
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// macFromTopic extracts the device identifier from a topic.
// tm/AA:BB:CC:DD:EE:FF/status → AA:BB:CC:DD:EE:FF
// tankmonitor/home/status     → home (legacy)
func macFromTopic(topic string) string {
	parts := strings.Split(topic, "/")
	if len(parts) >= 3 {
		return parts[1]
	}
	return ""
}

// isRealMAC returns true if the string looks like a MAC address (AA:BB:CC:DD:EE:FF).
func isRealMAC(s string) bool {
	if len(s) != 17 {
		return false
	}
	for i, c := range s {
		if i%3 == 2 {
			if c != ':' {
				return false
			}
		} else {
			if !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
				return false
			}
		}
	}
	return true
}
