import React, { useCallback, useEffect, useRef, useState } from 'react'
import type { Dayjs } from 'dayjs'
import dayjs from 'dayjs'
import {
  Alert, Badge, Button, Card, ConfigProvider, DatePicker, Form, Input, InputNumber,
  Modal, Popconfirm, Progress, Select, Space, Switch, Table, Tag, TimePicker,
  Typography, Upload, theme as antTheme, type TableColumnsType,
} from 'antd'
import {
  WifiOutlined, ClockCircleOutlined, HistoryOutlined,
  PlusOutlined, DeleteOutlined, ClearOutlined, EditOutlined,
  SyncOutlined, PoweroffOutlined,
  UserOutlined, LockOutlined, LogoutOutlined,
  UploadOutlined, ThunderboltOutlined, RollbackOutlined,
} from '@ant-design/icons'
import type { Schedule, Status, ControlCmd, OtaStatus } from './types'
import { login, sendControl, fetchOtaStatus, uploadFirmware, triggerOta, triggerRollback, fetchDeviceLogs, fetchDeviceHistory, clearDeviceHistory } from './api'

const { Text } = Typography

const WEB_APP_VERSION = '2.8.0'

// ---------------------------------------------------------------------------
// Login page
// ---------------------------------------------------------------------------

interface LoginPageProps {
  onLogin: (token: string) => void
}

function LoginPage({ onLogin }: LoginPageProps) {
  const [loading, setLoading] = useState(false)
  const [error,   setError]   = useState<string | null>(null)
  const [form] = Form.useForm<{ username: string; password: string }>()
  const T = THEMES[loadTheme()]
  const cardStyle: React.CSSProperties = {
    background: T.cardBg, border: `1px solid ${T.cardBd}`, borderRadius: T.cardRadius, clipPath: T.clipPath,
  }

  const onFinish = async (values: { username: string; password: string }) => {
    setLoading(true)
    setError(null)
    try {
      const token = await login(values.username, values.password)
      localStorage.setItem('auth_token', token)
      onLogin(token)
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'Login failed')
    } finally {
      setLoading(false)
    }
  }

  return (
    <ConfigProvider theme={{ algorithm: antTheme.darkAlgorithm, token: { colorPrimary: T.accent, borderRadius: T.cardRadius > 10 ? 14 : 6, fontFamily: T.bodyFont } }}>
      <div style={{
        minHeight: '100vh', background: T.bg,
        display: 'flex', alignItems: 'center', justifyContent: 'center',
        padding: 24, fontFamily: T.bodyFont,
      }}>
        <div style={{
          ...cardStyle, padding: '32px 28px', width: '100%', maxWidth: 360,
        }}>
          <div style={{ textAlign: 'center', marginBottom: 24 }}>
            <div style={{
              width: 14, height: 14, margin: '0 auto', background: T.accent,
              boxShadow: `0 0 14px ${T.accent}`, clipPath: 'polygon(50% 0,100% 50%,50% 100%,0 50%)',
            }} />
            <div style={{
              fontSize: 18, fontWeight: 700, color: T.accent, marginTop: 12,
              fontFamily: T.headingFont, letterSpacing: 1.5, textShadow: `0 0 12px ${T.accent}90`,
            }}>
              TANK MONITOR
            </div>
            <div style={{ fontSize: 11.5, color: T.labelClr, marginTop: 6, fontFamily: T.monoFont, letterSpacing: 0.5 }}>
              SIGN IN TO CONTINUE
            </div>
          </div>

          {error && (
            <Alert message={error} type="error" showIcon style={{ marginBottom: 16 }} />
          )}

          <Form form={form} layout="vertical" onFinish={onFinish}>
            <Form.Item name="username" label="Username" rules={[{ required: true, message: 'Enter username' }]}>
              <Input prefix={<UserOutlined />} placeholder="admin" autoComplete="username" />
            </Form.Item>
            <Form.Item name="password" label="Password" rules={[{ required: true, message: 'Enter password' }]}>
              <Input.Password prefix={<LockOutlined />} placeholder="Password" autoComplete="current-password" />
            </Form.Item>
            <Form.Item style={{ marginBottom: 0 }}>
              <Button
                type="primary" htmlType="submit" loading={loading} block
                style={{ fontFamily: T.monoFont, fontWeight: 700, letterSpacing: 1 }}
              >
                SIGN IN
              </Button>
            </Form.Item>
          </Form>
        </div>
      </div>
    </ConfigProvider>
  )
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function formatUptime(s: number): string {
  if (s < 60)   return `${s}s`
  if (s < 3600) return `${Math.floor(s / 60)}m ${s % 60}s`
  return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`
}

/** Convert 24-hr "HH:MM" or "HH:MM:SS" to "H:MM AM/PM" */
function to12hr(t: string): string {
  try {
    const parts = t.split(':')
    let h = parseInt(parts[0], 10)
    const m = parts[1].padStart(2, '0')
    const ampm = h >= 12 ? 'PM' : 'AM'
    h = h % 12 || 12
    return `${h}:${m} ${ampm}`
  } catch {
    return t
  }
}

/** Return the schedule index (i) of the next upcoming schedule for a motor */
function getNextSchedIdx(schedules: Schedule[], motor: string, currentTime: string): number | null {
  const filtered = schedules.filter(s => s.m === motor)
  if (!filtered.length || !currentTime) return null
  const [ch, cm] = currentTime.split(':').map(Number)
  const nowMins = ch * 60 + (cm || 0)
  let nextIdx: number | null = null
  let minDiff = Infinity
  for (const sch of filtered) {
    const [sh, sm] = sch.t.split(':').map(Number)
    let diff = sh * 60 + sm - nowMins
    if (diff <= 0) diff += 24 * 60
    if (diff < minDiff) { minDiff = diff; nextIdx = sch.i }
  }
  return nextIdx
}

// ---------------------------------------------------------------------------
// Sci-fi theme tokens — user picks between two full re-skins via a switcher
// ---------------------------------------------------------------------------

type ThemeName = 'hud' | 'quantum'

interface ThemeTokens {
  label:       string
  accent:      string
  bg:          string
  cardBg:      string
  cardBd:      string
  rowBd:       string
  labelClr:    string
  textClr:     string
  heroBg:      string
  cardRadius:  number
  clipPath?:   string
  scanline?:   boolean
  headingFont: string
  bodyFont:    string
  monoFont:    string
}

const THEMES: Record<ThemeName, ThemeTokens> = {
  hud: {
    label: 'HUD',
    accent: '#5eead4',
    bg: 'linear-gradient(rgba(4,14,16,.94),rgba(4,14,16,.97)), repeating-linear-gradient(0deg, rgba(94,234,212,.035) 0px, rgba(94,234,212,.035) 1px, transparent 1px, transparent 3px), #040e10',
    cardBg: 'rgba(8,22,25,.85)',
    cardBd: '#1c3b42',
    rowBd: '#12282d',
    labelClr: '#5a8a90',
    textClr: '#e8f7f5',
    heroBg: 'rgba(8,22,25,.85)',
    cardRadius: 4,
    clipPath: 'polygon(10px 0,100% 0,100% calc(100% - 10px),calc(100% - 10px) 100%,0 100%,0 10px)',
    scanline: true,
    headingFont: "'Orbitron', sans-serif",
    bodyFont: "'Rajdhani', Arial, sans-serif",
    monoFont: "'Share Tech Mono', monospace",
  },
  quantum: {
    label: 'Quantum',
    accent: '#4d8dff',
    bg: 'radial-gradient(ellipse 900px 500px at 50% -10%, rgba(77,141,255,.16), transparent 60%), #04060d',
    cardBg: 'rgba(255,255,255,.03)',
    cardBd: 'rgba(77,141,255,.22)',
    rowBd: 'rgba(77,141,255,.14)',
    labelClr: '#6d84a8',
    textClr: '#ffffff',
    heroBg: 'linear-gradient(160deg, rgba(77,141,255,.10), rgba(139,92,246,.05))',
    cardRadius: 20,
    scanline: false,
    headingFont: "'Orbitron', sans-serif",
    bodyFont: "'Rajdhani', Arial, sans-serif",
    monoFont: "'Share Tech Mono', monospace",
  },
}

const THEME_STORAGE_KEY = 'tm_theme'

function loadTheme(): ThemeName {
  const v = typeof localStorage !== 'undefined' ? localStorage.getItem(THEME_STORAGE_KEY) : null
  return v === 'hud' || v === 'quantum' ? v : 'quantum'
}

// ---------------------------------------------------------------------------
// SVG arc tank level circle (mirrors ESP32 webserver UI)
// ---------------------------------------------------------------------------

function TankCircle({ state, darkMode, size = 110, accent = '#6366f1', trackColor }: {
  state: string; darkMode: boolean; size?: number; accent?: string; trackColor?: string
}) {
  const r    = size / 110 * 45
  const circ = 2 * Math.PI * r  // ≈ 283 at size=110

  let pct   = 0
  let color = '#8c8c8c'

  if      (state === 'FULL')  { pct = 1.0; color = accent }
  else if (state === 'HALF')  { pct = 0.6; color = accent }
  else if (state === 'LOW')   { pct = 0.3; color = '#fa8c16' }
  else if (state === 'EMPTY') { pct = 0.0; color = '#ff4d4f' }

  const dash   = pct * circ
  const arcBg  = trackColor ?? (darkMode ? '#201f26' : '#e8e8e8')
  const cx = size / 2, cy = size / 2
  const strokeW = size / 110 * 9

  return (
    <div style={{ position: 'relative', display: 'inline-block', width: size, height: size, marginBottom: size >= 100 ? 8 : 0 }}>
      <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`} style={{ transform: 'rotate(-90deg)' }}>
        <circle cx={cx} cy={cy} r={r} fill="none" stroke={arcBg} strokeWidth={strokeW} />
        <circle
          cx={cx} cy={cy} r={r} fill="none"
          stroke={color} strokeWidth={strokeW} strokeLinecap="round"
          strokeDasharray={`${dash} ${circ}`}
          style={{ transition: 'stroke-dasharray 0.5s, stroke 0.5s', filter: `drop-shadow(0 0 6px ${color}88)` }}
        />
      </svg>
      <div style={{
        position: 'absolute', top: '50%', left: '50%',
        transform: 'translate(-50%, -50%)',
        fontSize: size >= 100 ? 13 : 11, fontWeight: 700, color,
        textAlign: 'center', lineHeight: 1.2,
      }}>
        {state || '--'}
      </div>
    </div>
  )
}

// ---------------------------------------------------------------------------
// Bento hero row: compact horizontal tank layout (ring + name + Power button)
// used inside the Dashboard hero card, replacing the old side-by-side
// TankCard pair with the denser "bento grid" look.
// ---------------------------------------------------------------------------

interface BentoTankRowProps {
  title:     string
  tankState: string
  motorOn:   boolean
  onOn:      () => void
  onOff:     () => void
  darkMode:  boolean
  divider:   boolean
  accent?:   string
  labelFont?: string
  nameFont?:  string
}

function BentoTankRow({ title, tankState, motorOn, onOn, onOff, darkMode, divider, accent = '#6366f1', labelFont, nameFont }: BentoTankRowProps) {
  const rowBd  = darkMode ? '#211f2c' : '#ece9f5'
  const nameCl = darkMode ? '#ffffff' : '#1a1a2e'
  const subCl  = darkMode ? '#7a7a85' : '#8a8a95'
  return (
    <div style={{
      display: 'flex', alignItems: 'center', gap: 12,
      marginTop: divider ? 16 : 6, paddingTop: divider ? 14 : 0,
      borderTop: divider ? `1px solid ${rowBd}` : 'none',
    }}>
      <TankCircle state={tankState} darkMode={darkMode} size={52} accent={accent} />
      <div style={{ flex: 1, minWidth: 0 }}>
        <div style={{ fontSize: 14, fontWeight: 700, color: nameCl, fontFamily: nameFont }}>{title}</div>
        <div style={{ fontSize: 10.5, color: subCl, marginTop: 2, fontFamily: labelFont, letterSpacing: .5 }}>
          {(tankState || '—').toUpperCase()} · MOTOR {motorOn ? 'ON' : 'OFF'}
        </div>
      </div>
      <Button
        size="small"
        type={motorOn ? 'default' : 'primary'}
        danger={motorOn}
        style={!motorOn ? { background: accent, borderColor: accent, color: '#04060d', fontWeight: 700 } : { fontWeight: 700 }}
        onClick={motorOn ? onOff : onOn}
      >
        {motorOn ? 'Power Off' : 'Power On'}
      </Button>
    </div>
  )
}

// ---------------------------------------------------------------------------
// Form types for Schedule modals
// ---------------------------------------------------------------------------

interface AddForm {
  motor:    0 | 1
  time:     Dayjs
  duration: number
}

type EditForm = AddForm

// ---------------------------------------------------------------------------
// Main component
// ---------------------------------------------------------------------------

export default function App() {
  const [token,      setToken]      = useState<string | null>(() => localStorage.getItem('auth_token'))
  const [backendVersion, setBackendVersion] = useState<string | null>(null)
  const [status,     setStatus]     = useState<Status | null>(null)
  const [connected,  setConnected]  = useState(false)
  const [lastUpdate, setLastUpdate] = useState<Date | null>(null)
  const [addOpen,    setAddOpen]    = useState(false)
  const [editRow,    setEditRow]    = useState<Schedule | null>(null)
  const [ctrlError,  setCtrlError]  = useState<string | null>(null)
  const [themeName,  setThemeName]  = useState<ThemeName>(loadTheme)
  const darkMode = true   // both sci-fi themes are dark-only
  const [form]     = Form.useForm<AddForm>()
  const [editForm] = Form.useForm<EditForm>()
  // OTA state
  const [otaStatus,    setOtaStatus]    = useState<OtaStatus | null>(null)
  const [uploadPct,    setUploadPct]    = useState<number | null>(null)
  const [otaError,     setOtaError]     = useState<string | null>(null)
  const [otaBusy,      setOtaBusy]      = useState(false)
  const [otaElapsed,   setOtaElapsed]   = useState(0)   // seconds since OTA triggered
  const otaPollRef      = useRef<ReturnType<typeof setInterval> | null>(null)
  const otaCountdownRef = useRef<ReturnType<typeof setInterval> | null>(null)
  // Device logs state
  const [deviceLogs,   setDeviceLogs]   = useState<string[]>([])
  const [logsAt,       setLogsAt]       = useState<string | null>(null)
  const [logsLoading,  setLogsLoading]  = useState(false)
  // History (date-range) state
  const [historyRecords, setHistoryRecords] = useState<Record<string, unknown>[]>([])
  const [historyLoading, setHistoryLoading] = useState(false)
  const [historyRange,   setHistoryRange]   = useState<[Dayjs, Dayjs]>([dayjs().subtract(7, 'day').startOf('day'), dayjs().endOf('day')])
  // MQTT credential change
  const [mqttPassInput, setMqttPassInput] = useState('')
  const [mqttPassBusy,  setMqttPassBusy]  = useState(false)
  const wsRef = useRef<WebSocket | null>(null)
  // Optimistic settings — key → {value, expiresAt ms} to win against stale WS updates
  const pendingRef = useRef<Map<string, { value: unknown; expiresAt: number }>>(new Map())

  const handleLogout = () => {
    localStorage.removeItem('auth_token')
    setToken(null)
    wsRef.current?.close()
    setConnected(false)
    setStatus(null)
    setBackendVersion(null)
  }

  const loadOtaStatus = useCallback(() => {
    if (!token) return
    fetchOtaStatus(token)
      .then(setOtaStatus)
      .catch((e: Error) => { if (e.message !== 'SESSION_EXPIRED') console.warn('OTA status:', e) })
  }, [token])

  useEffect(() => { loadOtaStatus() }, [loadOtaStatus])

  useEffect(() => { localStorage.setItem(THEME_STORAGE_KEY, themeName) }, [themeName])

  const loadDeviceLogs = useCallback(() => {
    if (!token) return
    setLogsLoading(true)
    fetchDeviceLogs(token)
      .then((data) => {
        setDeviceLogs(data.logs ?? [])
        setLogsAt(data.received_at ?? null)
      })
      .catch((e: Error) => { if (e.message !== 'SESSION_EXPIRED') console.warn('Logs:', e) })
      .finally(() => setLogsLoading(false))
  }, [token])

  const loadHistory = useCallback(() => {
    if (!token) return
    setHistoryLoading(true)
    const [from, to] = historyRange
    fetchDeviceHistory(token, from.unix(), to.unix())
      .then((data) => {
        const parsed = (data.records ?? []).map((r) => {
          try { return JSON.parse(r) as Record<string, unknown> } catch { return null }
        }).filter((r): r is Record<string, unknown> => r !== null)
        setHistoryRecords(parsed)
      })
      .catch((e: Error) => { if (e.message !== 'SESSION_EXPIRED') console.warn('History:', e) })
      .finally(() => setHistoryLoading(false))
  }, [token, historyRange])

  useEffect(() => { loadHistory() }, [loadHistory])

  // ── WebSocket connection ──────────────────────────────────────────────────
  useEffect(() => {
    if (!token) return
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
    const connect = () => {
      const ws = new WebSocket(`${proto}//${location.host}/ws?token=${encodeURIComponent(token)}`)
      ws.onopen    = () => {
        setConnected(true)
        // Fetch backend version (may differ from frontend build constant)
        fetch('/api/version', { headers: { Authorization: `Bearer ${token}` } })
          .then(r => r.ok ? r.json() : null)
          .then(d => { if (d?.web_version) setBackendVersion(d.web_version as string) })
          .catch(() => {})
      }
      ws.onclose   = (e) => { setConnected(false); if (e.code === 4001) { handleLogout(); return } setTimeout(connect, 3000) }
      ws.onerror   = () => ws.close()
      ws.onmessage = ({ data }) => {
        try {
          const raw = JSON.parse(data as string) as Record<string, unknown>
          const now = Date.now()
          for (const [k, p] of pendingRef.current) {
            if (now < p.expiresAt) raw[k] = p.value
            else pendingRef.current.delete(k)
          }
          setStatus(raw as unknown as Status)
          setLastUpdate(new Date())
        } catch { /* ignore malformed */ }
      }
      wsRef.current = ws
    }
    connect()
    return () => { wsRef.current?.close() }
  }, [token])

  // ── Show login page if not authenticated ──────────────────────────────────
  if (!token) {
    return <LoginPage onLogin={setToken} />
  }

  // ── Control helper ────────────────────────────────────────────────────────
  const ctrl = (cmd: ControlCmd) =>
    sendControl(cmd, token).catch((e: Error) => {
      if (e.message === 'SESSION_EXPIRED') { handleLogout(); return }
      setCtrlError(e.message)
      setTimeout(() => setCtrlError(null), 4000)
    })

  // Like ctrl() but also stores an optimistic override for `statusKey` so the UI
  // doesn't flicker back to the old value when the next WS status arrives before
  // the ESP32 has processed the change. The override expires after 4 seconds.
  const ctrlSetting = (cmd: ControlCmd, statusKey: string, value: unknown) => {
    pendingRef.current.set(statusKey, { value, expiresAt: Date.now() + 4000 })
    // Immediately reflect the change in the UI — don't wait for the next WS message
    setStatus(prev => prev ? { ...prev, [statusKey]: value } as Status : prev)
    ctrl(cmd)
  }

  // ── Schedule table columns ────────────────────────────────────────────────
  const schedCols: TableColumnsType<Schedule> = [
    { title: '#',        dataIndex: 'i', width: 40 },
    {
      title: 'Motor', dataIndex: 'm', width: 70,
      render: (m: string) => <Tag color={m === 'OH' ? 'blue' : 'purple'}>{m}</Tag>,
    },
    { title: 'Time', dataIndex: 't', width: 90, render: (t: string) => to12hr(t) },
    { title: 'Duration', dataIndex: 'd', render: (d: number) => `${d} min` },
    {
      title: '', key: 'actions', width: 80,
      render: (_: unknown, r: Schedule) => (
        <Space size={2}>
          <Button
            type="text" size="small" icon={<EditOutlined />}
            onClick={() => {
              setEditRow(r)
              editForm.setFieldsValue({
                motor: r.m === 'OH' ? 0 : 1,
                time: dayjs(r.t, 'HH:mm'),
                duration: r.d,
              })
            }}
          />
          <Popconfirm
            title="Remove this schedule?"
            onConfirm={() => ctrl({ cmd: 'sched_remove', index: r.i })}
            okText="Yes" cancelText="No"
          >
            <Button type="text" danger size="small" icon={<DeleteOutlined />} />
          </Popconfirm>
        </Space>
      ),
    },
  ]

  // ── Add schedule form submit ──────────────────────────────────────────────
  const onAdd = async (v: AddForm) => {
    await ctrl({ cmd: 'sched_add', motor: v.motor, time: v.time.format('HH:mm'), duration: v.duration })
    setAddOpen(false)
    form.resetFields()
  }

  // ── Edit schedule form submit ─────────────────────────────────────────────
  const onEdit = async (v: EditForm) => {
    if (!editRow) return
    // Delete old then add new — atomic from user perspective
    await ctrl({ cmd: 'sched_remove', index: editRow.i })
    await ctrl({ cmd: 'sched_add', motor: v.motor, time: v.time.format('HH:mm'), duration: v.duration })
    setEditRow(null)
    editForm.resetFields()
  }

  // ── OTA handlers ─────────────────────────────────────────────────────────
  const onOtaUpload = async (file: File) => {
    if (!token) return
    setOtaError(null)
    setUploadPct(0)
    setOtaBusy(true)
    try {
      await uploadFirmware(file, token, setUploadPct)
      await loadOtaStatus()
    } catch (e: unknown) {
      setOtaError(e instanceof Error ? e.message : 'Upload failed')
    } finally {
      setUploadPct(null)
      setOtaBusy(false)
    }
  }

  const onOtaTrigger = async () => {
    if (!token) return
    setOtaError(null)
    setOtaBusy(true)
    setOtaElapsed(0)
    try {
      await triggerOta(token)
      // 150-second countdown timer
      if (otaCountdownRef.current) clearInterval(otaCountdownRef.current)
      otaCountdownRef.current = setInterval(() => {
        setOtaElapsed(prev => prev + 1)
      }, 1000)
      // Poll for phase completion every 5 s
      if (otaPollRef.current) clearInterval(otaPollRef.current)
      otaPollRef.current = setInterval(async () => {
        try {
          const st = await fetchOtaStatus(token!)
          setOtaStatus(st)
          const done = !st.phase || st.phase === 'idle' || st.phase === 'success' || st.phase === 'failed'
          if (done) {
            clearInterval(otaPollRef.current!)
            otaPollRef.current = null
            clearInterval(otaCountdownRef.current!)
            otaCountdownRef.current = null
            setOtaBusy(false)
          }
        } catch {
          clearInterval(otaPollRef.current!)
          otaPollRef.current = null
          clearInterval(otaCountdownRef.current!)
          otaCountdownRef.current = null
          setOtaBusy(false)
        }
      }, 5000)
    } catch (e: unknown) {
      setOtaError(e instanceof Error ? e.message : 'Trigger failed')
      setOtaBusy(false)
    }
  }

  const onOtaRollback = async () => {
    if (!token) return
    setOtaError(null)
    setOtaBusy(true)
    try {
      await triggerRollback(token)
    } catch (e: unknown) {
      setOtaError(e instanceof Error ? e.message : 'Rollback failed')
    } finally {
      setOtaBusy(false)
    }
  }

  const s = status

  // All schedules shown; next upcoming per motor for row highlight
  const activeSchedules = s?.schedules ?? []
  const nextOH = s?.time ? getNextSchedIdx(activeSchedules, 'OH', s.time) : null
  const nextUG = s?.time ? getNextSchedIdx(activeSchedules, 'UG', s.time) : null

  // ── System info rows ──────────────────────────────────────────────────────
  const sysRows: [string, React.ReactNode][] = [
    ['WiFi',        s?.wifi_rssi != null ? `${s.wifi_rssi} dBm` : '—'],
    ['LoRa',        <Tag color={s?.lora_ok ? 'success' : (s ? 'error' : 'default')}>{s?.lora_ok ? 'OK' : (s ? 'FAIL' : '—')}</Tag>],
    ['Transmitter', <Tag color={s?.tx_lost === false ? 'success' : s?.tx_lost === true ? 'error' : 'default'}>{s?.tx_lost === false ? 'OK' : s?.tx_lost === true ? 'LOST' : '—'}</Tag>],
    ['Uptime',      s ? formatUptime(s.uptime_s) : '—'],
    ['Firmware',    s?.fw ?? '—'],
    ['TX Firmware', s?.tx_fw ?? '—'],
    ['Web App',     backendVersion ?? WEB_APP_VERSION],
    ['Last update', lastUpdate ? lastUpdate.toLocaleTimeString() : '—'],
  ]

  const T        = THEMES[themeName]
  const bg       = T.bg
  const cardBg   = T.cardBg
  const cardBd   = T.cardBd
  const rowBd    = T.rowBd
  const labelClr = T.labelClr
  const accent   = T.accent
  const cardStyle: React.CSSProperties = {
    background: cardBg, border: `1px solid ${cardBd}`, borderRadius: T.cardRadius,
    clipPath: T.clipPath, fontFamily: T.bodyFont,
  }
  const cardTitleStyle: React.CSSProperties = {
    fontSize: 11, color: labelClr, textTransform: 'uppercase', letterSpacing: 1.5, fontFamily: T.monoFont,
  }
  const heroCardStyle: React.CSSProperties = { ...cardStyle, background: T.heroBg }
  const statCardStyle: React.CSSProperties = { ...cardStyle }

  // ── Render ────────────────────────────────────────────────────────────────
  return (
    <ConfigProvider theme={{
      algorithm: antTheme.darkAlgorithm,
      token: { colorPrimary: accent, borderRadius: T.cardRadius > 10 ? 14 : 6, fontFamily: T.bodyFont },
    }}>
      <div style={{ minHeight: '100vh', background: bg, padding: '16px 14px', fontFamily: T.bodyFont }}>
        <div style={{ maxWidth: 560, margin: '0 auto', position: 'relative' }}>

        {/* ── Header: brand + theme switcher + section nav pills + live/logout ── */}
        <div style={{
          display: 'flex', alignItems: 'center', justifyContent: 'space-between',
          marginBottom: 20, paddingBottom: 12, flexWrap: 'wrap', gap: 10,
          borderBottom: `1px solid ${cardBd}`,
        }}>
          <span style={{
            fontSize: 14, fontWeight: 700, color: accent, display: 'flex', alignItems: 'center', gap: 8,
            fontFamily: T.headingFont, letterSpacing: 1.5, textShadow: `0 0 12px ${accent}90`,
          }}>
            <span style={{ width: 8, height: 8, background: accent, boxShadow: `0 0 10px ${accent}`, clipPath: 'polygon(50% 0,100% 50%,50% 100%,0 50%)' }} />
            TANK MONITOR
          </span>
          <div style={{
            display: 'flex', gap: 2, background: cardBg,
            border: `1px solid ${cardBd}`, borderRadius: 10, padding: 3,
          }}>
            {(['dashboard', 'history', 'settings', 'system'] as const).map(sec => (
              <a
                key={sec}
                href={`#${sec}`}
                style={{
                  padding: '6px 11px', borderRadius: 7, fontSize: 10, fontWeight: 700,
                  textTransform: 'uppercase', textDecoration: 'none', letterSpacing: 0.5,
                  color: labelClr, fontFamily: T.monoFont,
                }}
              >
                {sec}
              </a>
            ))}
          </div>
          <Space size={10}>
            {s?.time && (
              <Text style={{ color: labelClr, fontSize: 11.5, fontFamily: T.monoFont }}>
                <ClockCircleOutlined /> {to12hr(s.time)}
              </Text>
            )}
            <Badge
              status={connected ? 'success' : 'error'}
              text={<span style={{ color: labelClr, fontSize: 11.5, fontFamily: T.monoFont, letterSpacing: 0.5 }}>{connected ? 'LIVE' : 'OFFLINE'}</span>}
            />
            <div style={{ display: 'flex', gap: 2, background: cardBg, border: `1px solid ${cardBd}`, borderRadius: 8, padding: 2 }}>
              {(Object.keys(THEMES) as ThemeName[]).map(k => (
                <button
                  key={k}
                  onClick={() => setThemeName(k)}
                  style={{
                    border: 'none', cursor: 'pointer', padding: '4px 10px', borderRadius: 6,
                    fontSize: 9.5, fontWeight: 700, letterSpacing: 0.6, textTransform: 'uppercase',
                    fontFamily: T.monoFont,
                    background: themeName === k ? THEMES[k].accent : 'transparent',
                    color: themeName === k ? '#04060d' : labelClr,
                  }}
                  title={`Switch to ${THEMES[k].label} theme`}
                >
                  {THEMES[k].label}
                </button>
              ))}
            </div>
            <Button
              size="small"
              type="text"
              danger
              icon={<LogoutOutlined />}
              onClick={handleLogout}
              title="Sign out"
            >
              Logout
            </Button>
          </Space>
        </div>

        {/* ── Banners ── */}
        {!connected && (
          <Alert message="Disconnected — reconnecting…" type="warning" showIcon style={{ marginBottom: 12 }} />
        )}
        {ctrlError && (
          <Alert message={ctrlError} type="error" showIcon closable style={{ marginBottom: 12 }} />
        )}
        {s?.tx_lost && (
          <Alert
            message="Transmitter Lost — No signal from OH tank transmitter for over 90 seconds"
            type="warning" showIcon
            style={{ marginBottom: 12 }}
          />
        )}

        {/* ── Dashboard: hero orbit ring for Overhead + compact row for Underground + stat tiles ── */}
        <div id="dashboard" style={{ marginBottom: 12 }}>
          <div style={{ ...heroCardStyle, padding: 22, textAlign: 'center', position: 'relative', overflow: 'hidden', marginBottom: 12 }}>
            {T.scanline && (
              <div style={{
                position: 'absolute', inset: 0, pointerEvents: 'none', opacity: .5,
                background: 'linear-gradient(180deg,transparent,rgba(94,234,212,.06) 50%,transparent)',
              }} />
            )}
            <div style={{
              fontSize: 10, color: accent, fontWeight: 700, letterSpacing: 2, textTransform: 'uppercase',
              fontFamily: T.monoFont, marginBottom: 14, position: 'relative',
            }}>
              Overhead Tank
            </div>
            <div style={{ position: 'relative', display: 'inline-block' }}>
              {themeName === 'quantum' && (
                <div style={{ position: 'absolute', inset: -10, borderRadius: '50%', border: `1px dashed ${accent}55` }} />
              )}
              <TankCircle
                state={s?.oh_state ?? ''}
                darkMode={darkMode}
                accent={accent}
                trackColor={themeName === 'hud' ? 'rgba(94,234,212,.10)' : 'rgba(255,255,255,.08)'}
                size={140}
              />
            </div>
            <div style={{ fontSize: 13, color: T.textClr, fontFamily: T.bodyFont, marginTop: 4 }}>
              Motor {s?.oh_motor ? 'ON' : 'OFF'}
            </div>
            {s?.tx_lost && s?.oh_last_known && s.oh_last_known !== 'UNKNOWN' && (
              <div style={{ fontSize: 10.5, color: '#fa8c16', marginTop: 6, fontFamily: T.monoFont }}>
                Last known: {s.oh_last_known}
              </div>
            )}
            <div style={{ marginTop: 14, position: 'relative' }}>
              <Button
                style={
                  s?.oh_motor
                    ? { borderColor: '#ff6b6b', color: '#ff6b6b', fontFamily: T.monoFont, fontWeight: 700, letterSpacing: 1 }
                    : { background: accent, borderColor: accent, color: '#04060d', fontFamily: T.monoFont, fontWeight: 700, letterSpacing: 1 }
                }
                onClick={() => ctrl({ cmd: s?.oh_motor ? 'oh_off' : 'oh_on' })}
              >
                {s?.oh_motor ? 'POWER OFF' : 'POWER ON'}
              </Button>
            </div>
          </div>

          <div style={{ ...statCardStyle, padding: 16, marginBottom: 12 }}>
            <BentoTankRow
              title="Underground Tank"
              tankState={s?.ug_state ?? ''}
              motorOn={s?.ug_motor ?? false}
              onOn={()  => ctrl({ cmd: 'ug_on'  })}
              onOff={() => ctrl({ cmd: 'ug_off' })}
              darkMode={darkMode}
              divider={false}
              accent={accent}
              labelFont={T.monoFont}
              nameFont={T.bodyFont}
            />
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12 }}>
            <div style={{ ...statCardStyle, padding: 16 }}>
              <div style={cardTitleStyle}>System</div>
              <div style={{ fontSize: 20, fontWeight: 800, color: s?.tx_lost ? '#fa8c16' : '#34d399', marginTop: 8, fontFamily: T.headingFont }}>
                {s?.tx_lost ? 'ALERT' : 'NOMINAL'}
              </div>
              <div style={{ fontSize: 10.5, color: labelClr, marginTop: 4, fontFamily: T.monoFont }}>
                LoRa {s?.lora_ok ? 'OK' : 'LOST'} · WiFi {s?.wifi_rssi ?? '—'}dBm
              </div>
            </div>
            <div style={{ ...statCardStyle, padding: 16 }}>
              <div style={cardTitleStyle}>Uptime</div>
              <div style={{ fontSize: 20, fontWeight: 800, color: T.textClr, marginTop: 8, fontFamily: T.headingFont }}>
                {s ? formatUptime(s.uptime_s) : '—'}
              </div>
              <div style={{ fontSize: 10.5, color: labelClr, marginTop: 4, fontFamily: T.monoFont }}>
                FW v{s?.fw ?? '—'}
              </div>
            </div>
          </div>
        </div>

        {/* ── Schedules ── */}
        <Card
          size="small"
          title={<span style={cardTitleStyle}>Motor Scheduler</span>}
          style={{ ...cardStyle, marginBottom: 12 }}
          extra={
            <Space>
              <Button size="small" type="primary" icon={<PlusOutlined />} onClick={() => setAddOpen(true)}>
                Add
              </Button>
              <Popconfirm
                title="Clear all schedules?"
                onConfirm={() => ctrl({ cmd: 'sched_clear' })}
                okText="Yes" cancelText="No"
              >
                <Button size="small" danger icon={<ClearOutlined />}>Clear All</Button>
              </Popconfirm>
            </Space>
          }
        >
          <Table<Schedule>
            dataSource={activeSchedules}
            columns={schedCols}
            rowKey="i"
            size="small"
            pagination={false}
            locale={{ emptyText: 'No schedules configured' }}
            onRow={(record) => ({
              style: record.i === nextOH || record.i === nextUG
                ? { background: '#162312', borderLeft: '3px solid #52c41a' }
                : {},
            })}
          />
        </Card>

        {/* ── Settings ── */}
        <Card
          id="settings"
          size="small"
          title={<span style={cardTitleStyle}>Settings</span>}
          style={{ ...cardStyle, marginBottom: 12 }}
        >
          {([
            ['OH Display Only',          'oh_disp_only',  s?.oh_disp_only],
            ['UG Display Only',          'ug_disp_only',  s?.ug_disp_only],
            ['Ignore UG for OH Motor',   'ug_ignore',     s?.ug_ignore],
            ['Buzzer Delay Before Start','buzzer_delay',  s?.buzzer_delay],
            ['Stop Manual Motor on Full','manual_auto_stop', s?.manual_auto_stop],
          ] as [string, string, boolean | undefined][]).map(([label, key, val]) => (
            <div key={key} style={{
              display: 'flex', justifyContent: 'space-between', alignItems: 'center',
              padding: '7px 0', borderBottom: `1px solid ${rowBd}`, fontSize: 13,
            }}>
              <span>{label}</span>
              <Switch
                size="small"
                checked={val ?? false}
                disabled={!s}
                onChange={(checked) => ctrlSetting({ cmd: 'set_setting', key, value: checked }, key, checked)}
              />
            </div>
          ))}

          {/* LCD Backlight Mode */}
          <div style={{
            display: 'flex', justifyContent: 'space-between', alignItems: 'center',
            padding: '7px 0', borderBottom: `1px solid ${rowBd}`, fontSize: 13,
          }}>
            <span>LCD Backlight</span>
            <Select
              size="small"
              style={{ width: 170 }}
              disabled={!s}
              value={s?.lcd_bl_mode ?? 0}
              onChange={(v: number) => ctrlSetting(
                { cmd: 'set_lcd_mode', mode: ['auto', 'always_on', 'always_off'][v] } as unknown as ControlCmd,
                'lcd_bl_mode', v,
              )}
              options={[
                { value: 0, label: 'Auto' },
                { value: 1, label: 'On' },
                { value: 2, label: 'Off' },
              ]}
            />
          </div>

          {/* MQTT Password */}
          <div style={{
            display: 'flex', justifyContent: 'space-between', alignItems: 'center',
            padding: '7px 0', borderBottom: `1px solid ${rowBd}`, fontSize: 13, gap: 8,
          }}>
            <span>OH Motor Start Level</span>
            <Select
              size="small"
              style={{ width: 170 }}
              disabled={!s}
              value={s?.oh_start_level ?? 1}
              onChange={(v: number) => ctrlSetting(
                { cmd: 'set_setting', key: 'oh_start_level', value: v },
                'oh_start_level', v,
              )}
              options={[
                { value: 1, label: 'EMPTY' },
                { value: 2, label: 'LOW' },
                { value: 3, label: 'HALF' },
              ]}
            />
          </div>
          <div style={{
            display: 'flex', justifyContent: 'space-between', alignItems: 'center',
            padding: '7px 0', borderBottom: `1px solid ${rowBd}`, fontSize: 13, gap: 8,
          }}>
            <span>OH Motor Stop Level</span>
            <Select
              size="small"
              style={{ width: 170 }}
              disabled={!s}
              value={s?.oh_stop_level ?? 4}
              onChange={(v: number) => ctrlSetting(
                { cmd: 'set_setting', key: 'oh_stop_level', value: v },
                'oh_stop_level', v,
              )}
              options={[
                { value: 2, label: 'LOW' },
                { value: 3, label: 'HALF' },
                { value: 4, label: 'FULL' },
              ]}
            />
          </div>
          <div style={{
            display: 'flex', justifyContent: 'space-between', alignItems: 'center',
            padding: '7px 0', borderBottom: `1px solid ${rowBd}`, fontSize: 13, gap: 8,
          }}>
            <span>OH Max Motor Runtime</span>
            <InputNumber
              size="small"
              style={{ width: 170 }}
              disabled={!s}
              min={5}
              max={60}
              addonAfter="min"
              value={s?.oh_max_run_min ?? 20}
              onChange={(v) => {
                if (v == null) return
                ctrlSetting(
                  { cmd: 'set_setting', key: 'oh_max_run_min', value: v },
                  'oh_max_run_min', v,
                )
              }}
            />
          </div>
          <div style={{
            display: 'flex', justifyContent: 'space-between', alignItems: 'center',
            padding: '7px 0', borderBottom: `1px solid ${rowBd}`, fontSize: 13, gap: 8,
          }}>
            <span>Reboot if Unreachable</span>
            <InputNumber
              size="small"
              style={{ width: 170 }}
              disabled={!s}
              min={10}
              max={60}
              addonAfter="min"
              value={s?.mqtt_watchdog_min ?? 15}
              onChange={(v) => {
                if (v == null) return
                ctrlSetting(
                  { cmd: 'set_setting', key: 'mqtt_watchdog_min', value: v },
                  'mqtt_watchdog_min', v,
                )
              }}
            />
          </div>
          <div style={{
            display: 'flex', justifyContent: 'space-between', alignItems: 'center',
            padding: '7px 0', borderTop: `1px solid ${rowBd}`, fontSize: 13, gap: 8,
          }}>
            <span style={{ whiteSpace: 'nowrap' }}>MQTT Password</span>
            <Space.Compact style={{ flex: 1 }}>
              <Input.Password
                size="small"
                placeholder="New password"
                value={mqttPassInput}
                onChange={e => setMqttPassInput(e.target.value)}
                disabled={!s || mqttPassBusy}
                style={{ flex: 1 }}
              />
              <Popconfirm
                title="Update MQTT password on the device?"
                description="Do this before changing the broker password."
                onConfirm={async () => {
                  if (!mqttPassInput.trim()) return
                  setMqttPassBusy(true)
                  await ctrl({ cmd: 'set_mqtt_creds', pass: mqttPassInput.trim() })
                  setMqttPassInput('')
                  setMqttPassBusy(false)
                }}
                okText="Update" cancelText="Cancel"
              >
                <Button size="small" disabled={!s || !mqttPassInput.trim() || mqttPassBusy} loading={mqttPassBusy}>
                  Set
                </Button>
              </Popconfirm>
            </Space.Compact>
          </div>
        </Card>

        {/* ── Actions ── */}
        <Card
          size="small"
          title={<span style={cardTitleStyle}>Actions</span>}
          style={{ ...cardStyle, marginBottom: 12 }}
        >
          <Space wrap>
            <Button
              icon={<SyncOutlined />}
              disabled={!s}
              onClick={() => ctrl({ cmd: 'sync_ntp' })}
            >
              Sync NTP Time
            </Button>
            <Popconfirm
              title="Reboot the ESP32?"
              onConfirm={() => ctrl({ cmd: 'reboot' })}
              okText="Reboot" cancelText="Cancel"
              okButtonProps={{ danger: true }}
            >
              <Button icon={<PoweroffOutlined />} danger disabled={!s}>
                Reboot
              </Button>
            </Popconfirm>
          </Space>
        </Card>

        {/* ── Firmware OTA ── */}
        <Card
          size="small"
          title={<span style={cardTitleStyle}>Firmware Update (OTA)</span>}
          style={{ ...cardStyle, marginBottom: 12 }}
        >
          {otaError && (
            <Alert message={otaError} type="error" showIcon closable style={{ marginBottom: 10 }}
              onClose={() => setOtaError(null)} />
          )}

          {/* OTA phase progress */}
          {otaStatus?.phase && otaStatus.phase !== 'idle' && (
            <>
              <Alert
                style={{ marginBottom: 10 }}
                showIcon
                type={otaStatus.phase === 'success' ? 'success' : otaStatus.phase === 'failed' ? 'error' : 'info'}
                message={
                  otaStatus.phase === 'triggered'    ? '⚡ OTA triggered — waiting for ESP32 to acknowledge…' :
                  otaStatus.phase === 'ack_received' ? '✅ ESP32 confirmed — downloading firmware…' :
                  otaStatus.phase === 'downloading'  ? '⬇️ ESP32 is flashing firmware…' :
                  otaStatus.phase === 'success'      ? `✅ Update successful! ${otaStatus.prev_fw ? `${otaStatus.prev_fw} → ` : ''}${s?.fw ?? 'new version'}` :
                  otaStatus.phase === 'failed'       ? '❌ Update failed — ESP32 did not apply the firmware. Try again or use serial flash.' : ''
                }
              />
              {otaBusy && (
                <div style={{ marginBottom: 10 }}>
                  <div style={{ fontSize: 11, color: labelClr, marginBottom: 4 }}>
                    OTA in progress — {Math.max(0, 150 - otaElapsed)}s remaining
                  </div>
                  <Progress
                    percent={Math.min(100, Math.round((otaElapsed / 150) * 100))}
                    strokeColor={{ from: '#1890ff', to: '#52c41a' }}
                    status="active"
                    showInfo={false}
                  />
                </div>
              )}
            </>
          )}

          {/* Current staged firmware info */}
          {otaStatus?.has_firmware ? (
            <Alert
              type="info" showIcon style={{ marginBottom: 10 }}
              message={
                <span>
                  <strong>{otaStatus.filename}</strong>
                  {' — '}
                  {(otaStatus.size / 1024).toFixed(0)} KB
                  {' — uploaded '}
                  {new Date(otaStatus.uploaded_at).toLocaleString()}
                </span>
              }
            />
          ) : (
            <Alert type="warning" showIcon message="No firmware staged. Upload a .bin file to flash." style={{ marginBottom: 10 }} />
          )}

          {/* Upload progress */}
          {uploadPct !== null && (
            <Progress percent={uploadPct} style={{ marginBottom: 10 }} />
          )}

          <Space wrap>
            <Upload
              accept=".bin"
              showUploadList={false}
              beforeUpload={(file) => { void onOtaUpload(file); return false }}
            >
              <Button icon={<UploadOutlined />} loading={uploadPct !== null} disabled={otaBusy}>
                {uploadPct !== null ? `Uploading ${uploadPct}%` : 'Upload firmware.bin'}
              </Button>
            </Upload>

            <Popconfirm
              title="Flash this firmware to the ESP32?"
              description="The device will reboot and apply the update."
              onConfirm={onOtaTrigger}
              okText="Flash" cancelText="Cancel"
              disabled={!otaStatus?.has_firmware || otaBusy}
            >
              <Button
                icon={<ThunderboltOutlined />}
                type="primary"
                disabled={!otaStatus?.has_firmware || otaBusy}
                loading={otaBusy && uploadPct === null}
              >
                {otaBusy && uploadPct === null
                  ? (otaStatus?.phase === 'triggered'   ? 'Triggering…'
                  :  otaStatus?.phase === 'downloading' ? 'Downloading…'
                  :  'Flashing…')
                  : 'Flash to ESP32'}
              </Button>
            </Popconfirm>

            <Popconfirm
              title="Rollback to previous firmware?"
              description="ESP32 will reboot into the previous OTA partition."
              onConfirm={onOtaRollback}
              okText="Rollback" cancelText="Cancel"
              okButtonProps={{ danger: true }}
              disabled={!s || otaBusy}
            >
              <Button icon={<RollbackOutlined />} danger disabled={!s || otaBusy}>
                Rollback
              </Button>
            </Popconfirm>
          </Space>

          <div style={{ marginTop: 10, fontSize: 11, color: labelClr }}>
            Current firmware: <strong>{s?.fw ?? '—'}</strong>
            {' · '}Rollback reverts to the previous OTA partition on the ESP32.
          </div>
        </Card>

        {/* ── History (date-range: day / week / month) ── */}
        <Card
          id="history"
          size="small"
          title={<span style={cardTitleStyle}><HistoryOutlined style={{ marginRight: 6 }} />History</span>}
          style={{ ...cardStyle, marginBottom: 12 }}
          extra={
            <Popconfirm
              title="Clear history?"
              description="Records are archived, not deleted — still retrievable by date range."
              onConfirm={() => { if (token) clearDeviceHistory(token).then(loadHistory).catch(() => {}) }}
            >
              <Button size="small" danger icon={<ClearOutlined />}>Clear</Button>
            </Popconfirm>
          }
        >
          <Space wrap style={{ marginBottom: 10 }}>
            <DatePicker.RangePicker
              size="small"
              value={historyRange}
              onChange={(v) => { if (v && v[0] && v[1]) setHistoryRange([v[0], v[1]]) }}
              allowClear={false}
            />
            <Button size="small" onClick={() => setHistoryRange([dayjs().startOf('day'), dayjs().endOf('day')])}>Today</Button>
            <Button size="small" onClick={() => setHistoryRange([dayjs().subtract(7, 'day').startOf('day'), dayjs().endOf('day')])}>This Week</Button>
            <Button size="small" onClick={() => setHistoryRange([dayjs().subtract(30, 'day').startOf('day'), dayjs().endOf('day')])}>This Month</Button>
            <Button size="small" icon={<SyncOutlined spin={historyLoading} />} onClick={loadHistory}>Refresh</Button>
          </Space>
          <Table
            size="small"
            rowKey={(r) => `${r.ts}-${r.ev}`}
            loading={historyLoading}
            dataSource={historyRecords}
            pagination={{ pageSize: 10, size: 'small' }}
            columns={[
              { title: 'Time', dataIndex: 'time', key: 'time', width: 200 },
              { title: 'Event', dataIndex: 'ev', key: 'ev' },
              { title: 'Reason', dataIndex: 'rsnStr', key: 'rsnStr' },
            ]}
          />
        </Card>

        {/* ── Device Logs ── */}
        <Card
          size="small"
          title={<span style={cardTitleStyle}>Device Logs</span>}
          style={{ ...cardStyle, marginBottom: 12 }}
          extra={
            <Space size={6}>
              <Select
                size="small"
                value={s?.log_level ?? 'info'}
                disabled={!s}
                style={{ width: 160 }}
                onChange={(v: string) => ctrlSetting({ cmd: 'set_log_level', level: v } as ControlCmd, 'log_level', v)}
                options={[
                  { value: 'info',  label: 'Info (Warn + Error)' },
                  { value: 'debug', label: 'Debug (All)' },
                ]}
              />
              <Button size="small" icon={<SyncOutlined spin={logsLoading} />}
                disabled={!s || logsLoading}
                onClick={() => { ctrl({ cmd: 'get_logs' } as ControlCmd); setTimeout(loadDeviceLogs, 2000) }}
              >
                Refresh
              </Button>
            </Space>
          }
        >
          {logsAt && (
            <div style={{ fontSize: 11, color: labelClr, marginBottom: 6 }}>
              Last received: {new Date(logsAt).toLocaleString()}
            </div>
          )}
          <div style={{
            background: darkMode ? '#0d0d0d' : '#f5f5f5',
            border: `1px solid ${rowBd}`, borderRadius: 6,
            padding: '8px 10px', maxHeight: 240, overflowY: 'auto',
            fontFamily: 'monospace', fontSize: 11,
          }}>
            {deviceLogs.length === 0
              ? <span style={{ color: labelClr }}>No logs — press Refresh to load.</span>
              : [...deviceLogs].reverse().map((line, i) => (
                  <div key={i} style={{
                    color: line.includes('[WARN]') ? '#fa8c16'
                         : line.includes('[ERROR]') ? '#ff4d4f'
                         : darkMode ? '#b3b3b3' : '#333',
                    marginBottom: 2,
                    whiteSpace: 'pre-wrap', wordBreak: 'break-all',
                  }}>
                    {line}
                  </div>
                ))
            }
          </div>
        </Card>

        {/* ── System Info (bottom) ── */}
        <Card
          id="system"
          size="small"
          title={<span style={cardTitleStyle}>System</span>}
          style={cardStyle}
        >
          {sysRows.map(([label, value]) => (
            <div key={label} style={{
              display: 'flex', justifyContent: 'space-between', alignItems: 'center',
              padding: '6px 0', borderBottom: `1px solid ${rowBd}`, fontSize: 13,
            }}>
              <span style={{ color: labelClr }}>
                {label === 'WiFi' && <WifiOutlined style={{ marginRight: 4 }} />}
                {label}
              </span>
              <span style={{ fontWeight: 500 }}>{value}</span>
            </div>
          ))}
        </Card>

        {/* ── Add Schedule Modal ── */}
        <Modal
          title="Add Schedule"
          open={addOpen}
          onOk={() => form.submit()}
          onCancel={() => { setAddOpen(false); form.resetFields() }}
          okText="Add"
          destroyOnClose
        >
          <Form
            form={form}
            layout="vertical"
            onFinish={onAdd}
            initialValues={{ motor: 0, duration: 30 }}
          >
            <Form.Item name="motor" label="Motor" rules={[{ required: true }]}>
              <Select
                options={[
                  { value: 0, label: 'OH — Overhead' },
                  { value: 1, label: 'UG — Underground' },
                ]}
              />
            </Form.Item>
            <Form.Item name="time" label="Start Time" rules={[{ required: true, message: 'Please select a time' }]}>
              <TimePicker format="HH:mm" minuteStep={5} style={{ width: '100%' }} />
            </Form.Item>
            <Form.Item name="duration" label="Duration (minutes)" rules={[{ required: true }]}>
              <InputNumber min={1} max={480} style={{ width: '100%' }} addonAfter="min" />
            </Form.Item>
          </Form>
        </Modal>

        {/* ── Edit Schedule Modal ── */}
        <Modal
          title="Edit Schedule"
          open={editRow !== null}
          onOk={() => editForm.submit()}
          onCancel={() => { setEditRow(null); editForm.resetFields() }}
          okText="Save"
          destroyOnClose
        >
          <Form
            form={editForm}
            layout="vertical"
            onFinish={onEdit}
          >
            <Form.Item name="motor" label="Motor" rules={[{ required: true }]}>
              <Select
                options={[
                  { value: 0, label: 'OH — Overhead' },
                  { value: 1, label: 'UG — Underground' },
                ]}
              />
            </Form.Item>
            <Form.Item name="time" label="Start Time" rules={[{ required: true, message: 'Please select a time' }]}>
              <TimePicker format="HH:mm" minuteStep={5} style={{ width: '100%' }} />
            </Form.Item>
            <Form.Item name="duration" label="Duration (minutes)" rules={[{ required: true }]}>
              <InputNumber min={1} max={480} style={{ width: '100%' }} addonAfter="min" />
            </Form.Item>
          </Form>
        </Modal>

        </div>
      </div>
    </ConfigProvider>
  )
}
