import type { ControlCmd, OtaStatus } from './types'

export async function login(username: string, password: string): Promise<string> {
  const res = await fetch('/api/login', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username, password }),
  })
  if (!res.ok) throw new Error('Invalid username or password')
  const data = await res.json() as { token: string }
  return data.token
}

export async function sendControl(cmd: ControlCmd, token: string): Promise<void> {
  const res = await fetch('/api/control', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'Authorization': `Bearer ${token}`,
    },
    body: JSON.stringify(cmd),
  })
  if (res.status === 401) throw new Error('SESSION_EXPIRED')
  if (!res.ok) {
    const text = await res.text()
    throw new Error(`Control failed (${res.status}): ${text}`)
  }
}

export async function fetchOtaStatus(token: string): Promise<OtaStatus> {
  const res = await fetch('/api/ota/status', {
    headers: { 'Authorization': `Bearer ${token}` },
  })
  if (res.status === 401) throw new Error('SESSION_EXPIRED')
  if (!res.ok) throw new Error('Failed to fetch OTA status')
  return res.json() as Promise<OtaStatus>
}

export async function uploadFirmware(
  file: File,
  token: string,
  onProgress?: (pct: number) => void,
): Promise<void> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest()
    xhr.open('POST', '/api/ota/upload')
    xhr.setRequestHeader('Authorization', `Bearer ${token}`)
    if (onProgress) {
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) onProgress(Math.round((e.loaded / e.total) * 100))
      }
    }
    xhr.onload = () => {
      if (xhr.status === 401) { reject(new Error('SESSION_EXPIRED')); return }
      if (xhr.status >= 200 && xhr.status < 300) { resolve(); return }
      reject(new Error(`Upload failed (${xhr.status}): ${xhr.responseText}`))
    }
    xhr.onerror = () => reject(new Error('Upload failed'))
    const fd = new FormData()
    fd.append('firmware', file)
    xhr.send(fd)
  })
}

export async function triggerOta(token: string): Promise<void> {
  const res = await fetch('/api/ota/trigger', {
    method: 'POST',
    headers: { 'Authorization': `Bearer ${token}` },
  })
  if (res.status === 401) throw new Error('SESSION_EXPIRED')
  if (!res.ok) throw new Error('Failed to trigger OTA')
}

export async function triggerRollback(token: string): Promise<void> {
  const res = await fetch('/api/ota/rollback', {
    method: 'POST',
    headers: { 'Authorization': `Bearer ${token}` },
  })
  if (res.status === 401) throw new Error('SESSION_EXPIRED')
  if (!res.ok) throw new Error('Failed to trigger rollback')
}

export async function fetchDeviceLogs(token: string): Promise<{ logs: string[], received_at?: string, note?: string }> {
  const res = await fetch('/api/logs', {
    headers: { 'Authorization': `Bearer ${token}` },
  })
  if (res.status === 401) throw new Error('SESSION_EXPIRED')
  if (!res.ok) throw new Error('Failed to fetch logs')
  return res.json() as Promise<{ logs: string[], received_at?: string, note?: string }>
}

// fetchDeviceHistory — omit from/to for the default "live" view (last 1000,
// non-archived). Pass unix-second from/to to browse any date range, which
// also includes previously "cleared" (archived) records.
export async function fetchDeviceHistory(
  token: string, from?: number, to?: number,
): Promise<{ count: number; records: string[] }> {
  const qs = (from != null || to != null)
    ? `?${from != null ? `from=${from}` : ''}${to != null ? `&to=${to}` : ''}`
    : ''
  const res = await fetch(`/api/history${qs}`, {
    headers: { 'Authorization': `Bearer ${token}` },
  })
  if (res.status === 401) throw new Error('SESSION_EXPIRED')
  if (!res.ok) throw new Error('Failed to fetch history')
  const body = await res.json() as { data?: { count: number; records: string[] } }
  return body.data ?? { count: 0, records: [] }
}

// clearDeviceHistory — archives (no longer deletes) all history for the
// current device; archived rows remain retrievable via fetchDeviceHistory
// with an explicit from/to range.
export async function clearDeviceHistory(token: string): Promise<void> {
  const res = await fetch('/api/history', {
    method: 'DELETE',
    headers: { 'Authorization': `Bearer ${token}` },
  })
  if (res.status === 401) throw new Error('SESSION_EXPIRED')
  if (!res.ok) throw new Error('Failed to clear history')
}

