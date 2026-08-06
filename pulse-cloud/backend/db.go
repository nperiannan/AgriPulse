package main

import (
	"database/sql"
	"fmt"
	"log"

	_ "modernc.org/sqlite"
)

var db *sql.DB

const dbPath = "/data/tankmonitor.db"

func initDB() {
	var err error
	db, err = sql.Open("sqlite", dbPath+"?_journal_mode=WAL&_foreign_keys=on")
	if err != nil {
		log.Fatalf("[DB] open: %v", err)
	}
	if err = db.Ping(); err != nil {
		log.Fatalf("[DB] ping: %v", err)
	}
	migrate()
	seedDeviceTypes()
	log.Printf("[DB] Ready — %s", dbPath)
}

func migrate() {
	stmts := []string{
		`CREATE TABLE IF NOT EXISTS device_types (
			type_id      TEXT PRIMARY KEY,
			display_name TEXT NOT NULL,
			vendor       TEXT NOT NULL,
			capabilities TEXT NOT NULL DEFAULT '[]'
		)`,
		`CREATE TABLE IF NOT EXISTS users (
			user_id       INTEGER PRIMARY KEY AUTOINCREMENT,
			username      TEXT    UNIQUE NOT NULL,
			password_hash TEXT    NOT NULL,
			is_admin      INTEGER NOT NULL DEFAULT 0,
			created_at    TEXT    DEFAULT (datetime('now'))
		)`,
		`CREATE TABLE IF NOT EXISTS devices (
			mac          TEXT PRIMARY KEY,
			type_id      TEXT REFERENCES device_types(type_id),
			display_name TEXT,
			fw_version   TEXT,
			last_seen    TEXT,
			created_at   TEXT DEFAULT (datetime('now'))
		)`,
		`CREATE TABLE IF NOT EXISTS user_devices (
			user_id INTEGER NOT NULL REFERENCES users(user_id)  ON DELETE CASCADE,
			mac     TEXT    NOT NULL REFERENCES devices(mac)    ON DELETE CASCADE,
			role    TEXT    NOT NULL DEFAULT 'owner',
			PRIMARY KEY (user_id, mac)
		)`,
		// Index for fast per-user device lookups
		`CREATE INDEX IF NOT EXISTS idx_user_devices_user ON user_devices(user_id)`,
		`CREATE TABLE IF NOT EXISTS push_tokens (
			token      TEXT PRIMARY KEY,
			user_id    INTEGER NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
			updated_at TEXT
		)`,
		`CREATE INDEX IF NOT EXISTS idx_push_tokens_user ON push_tokens(user_id)`,
		`CREATE TABLE IF NOT EXISTS history_events (
			mac    TEXT    NOT NULL,
			ts     INTEGER NOT NULL,
			ev     TEXT    NOT NULL,
			record TEXT    NOT NULL,
			PRIMARY KEY (mac, ts, ev)
		)`,
		`CREATE INDEX IF NOT EXISTS idx_history_mac_ts ON history_events(mac, ts)`,
		// Delivery issues ("unacknowledged incidents" — motor commands the
		// controller never acked) are archived here by the app before it clears
		// its local SharedPreferences log, so they remain available for later
		// debugging instead of being lost forever.
		`CREATE TABLE IF NOT EXISTS delivery_issues_archive (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			mac         TEXT    NOT NULL,
			ts          INTEGER NOT NULL,
			motor       TEXT    NOT NULL,
			start       INTEGER NOT NULL,
			device_name TEXT,
			archived_at TEXT    DEFAULT (datetime('now'))
		)`,
		`CREATE INDEX IF NOT EXISTS idx_delivery_issues_mac_ts ON delivery_issues_archive(mac, ts)`,
	}
	for _, s := range stmts {
		if _, err := db.Exec(s); err != nil {
			log.Fatalf("[DB] migrate: %v\nSQL: %s", err, s)
		}
	}

	// history_events predates the archive feature (2026-07-19) — add the
	// column retroactively for existing databases instead of a destructive
	// table rebuild. "Clear history" now sets archived=1 instead of deleting
	// rows, so historical records stay queryable by date range afterwards.
	addColumnIfMissing("history_events", "archived", "INTEGER NOT NULL DEFAULT 0")
}

// addColumnIfMissing runs `ALTER TABLE ... ADD COLUMN` only if the column
// doesn't already exist, so it's safe to call on every startup.
func addColumnIfMissing(table, column, def string) {
	rows, err := db.Query(fmt.Sprintf("PRAGMA table_info(%s)", table))
	if err != nil {
		log.Fatalf("[DB] pragma table_info %s: %v", table, err)
	}
	exists := false
	for rows.Next() {
		var cid, notnull, pk int
		var name, ctype string
		var dflt interface{}
		if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err == nil && name == column {
			exists = true
		}
	}
	rows.Close()
	if exists {
		return
	}
	if _, err := db.Exec(fmt.Sprintf("ALTER TABLE %s ADD COLUMN %s %s", table, column, def)); err != nil {
		log.Fatalf("[DB] add column %s.%s: %v", table, column, err)
	}
	log.Printf("[DB] migrated: added column %s.%s", table, column)
}

func seedDeviceTypes() {
	types := []struct {
		id, name, vendor, caps string
	}{
		{
			"tank_monitor",
			"Tank Monitor",
			"AgriPulse",
			`["oh_motor","ug_motor","buzzer","float_switch","lora","schedules"]`,
		},
		{
			"smart_ups",
			"Smart UPS",
			"Generic",
			`["battery_pct","load_pct","output_voltage","runtime_min"]`,
		},
	}
	for _, t := range types {
		_, err := db.Exec(`
			INSERT INTO device_types(type_id, display_name, vendor, capabilities)
			VALUES(?,?,?,?)
			ON CONFLICT(type_id) DO NOTHING`,
			t.id, t.name, t.vendor, t.caps)
		if err != nil {
			log.Printf("[DB] seed device_type %s: %v", t.id, err)
		}
	}
}
