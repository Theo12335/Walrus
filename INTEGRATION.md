# Project Walrus — Mobile & Backend Integration Guide

## Overview

```
[ESP32] --POST sensor data--> [Vercel API] --insert--> [Supabase DB]
                                    |
                              reads commands
                                    |
                              returns in response
                                    ▲
[Expo App] --POST commands--> [Vercel /api/commands] --upsert--> [Supabase device_commands]
```

ESP32 syncs every **15 seconds**. Commands sent from the app take effect on the next sync.

---

## 1. Supabase Setup

### 1.1 Sensor Readings Table

```sql
CREATE TABLE sensor_readings (
    id                  BIGSERIAL PRIMARY KEY,
    device_id           TEXT NOT NULL,
    basin_temp          FLOAT,
    tds_ppm             INTEGER,
    clean_level_cm      FLOAT,
    intake_pump_active  BOOLEAN DEFAULT FALSE,
    collect_pump_active BOOLEAN DEFAULT FALSE,
    mist_active         BOOLEAN DEFAULT FALSE,
    float_water_detect  BOOLEAN DEFAULT FALSE,
    state               TEXT,
    created_at          TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_readings_device_time
    ON sensor_readings (device_id, created_at DESC);

CREATE VIEW latest_reading AS
    SELECT * FROM sensor_readings
    ORDER BY created_at DESC
    LIMIT 1;
```

### 1.2 Device Commands Table

```sql
CREATE TABLE device_commands (
    device_id             TEXT PRIMARY KEY,
    sleep                 BOOLEAN DEFAULT FALSE,
    intake_pump_override  TEXT DEFAULT 'auto',
    collect_pump_override TEXT DEFAULT 'auto',
    mist_override         TEXT DEFAULT 'auto',
    updated_at            TIMESTAMPTZ DEFAULT NOW()
);

-- Insert default row
INSERT INTO device_commands (device_id)
VALUES ('WALRUS_001');
```

### 1.3 Override Values

| Value | Meaning |
|---|---|
| `auto` | Sensor logic controls the actuator |
| `on` | Force ON regardless of sensors |
| `off` | Force OFF regardless of sensors |

---

## 2. Vercel API Setup

### 2.1 Environment Variables

Add these to your Vercel project settings under **Settings → Environment Variables**:

```
SUPABASE_URL=https://your-project.supabase.co
SUPABASE_SERVICE_KEY=your-service-role-key
ESP32_API_KEY=walrus-esp32-key-2026
```

### 2.2 ESP32 Data Endpoint — `pages/api/esp32/data.ts`

Receives sensor data from ESP32, saves to Supabase, returns current commands.

```ts
import { createClient } from '@supabase/supabase-js';
import type { NextApiRequest, NextApiResponse } from 'next';

const supabase = createClient(
  process.env.SUPABASE_URL!,
  process.env.SUPABASE_SERVICE_KEY!
);

export default async function handler(req: NextApiRequest, res: NextApiResponse) {
  if (req.method !== 'POST') return res.status(405).end();

  // Auth check
  const apiKey = req.headers['x-api-key'];
  if (apiKey !== process.env.ESP32_API_KEY)
    return res.status(401).json({ error: 'Unauthorized' });

  const { device_id, sensors, actuators, state } = req.body;

  // Save sensor reading
  const { error } = await supabase.from('sensor_readings').insert({
    device_id,
    basin_temp:          sensors?.basin_temp,
    tds_ppm:             sensors?.tds_ppm,
    clean_level_cm:      sensors?.clean_level_cm,
    intake_pump_active:  actuators?.intake_pump_active,
    collect_pump_active: actuators?.collect_pump_active,
    mist_active:         actuators?.mist_active,
    float_water_detect:  actuators?.float_water_detect,
    state,
  });

  if (error) return res.status(500).json({ error });

  // Read current commands and return to ESP32
  const { data: commands } = await supabase
    .from('device_commands')
    .select('*')
    .eq('device_id', device_id)
    .single();

  return res.status(201).json({
    sleep: commands?.sleep ?? false,
    commands: {
      intake_pump_override:  commands?.intake_pump_override  ?? 'auto',
      collect_pump_override: commands?.collect_pump_override ?? 'auto',
      mist_override:         commands?.mist_override         ?? 'auto',
    },
  });
}
```

### 2.3 Commands Endpoint — `pages/api/commands.ts`

Receives override commands from the mobile app.

```ts
import { createClient } from '@supabase/supabase-js';
import type { NextApiRequest, NextApiResponse } from 'next';

const supabase = createClient(
  process.env.SUPABASE_URL!,
  process.env.SUPABASE_SERVICE_KEY!
);

export default async function handler(req: NextApiRequest, res: NextApiResponse) {
  if (req.method !== 'POST') return res.status(405).end();

  const {
    device_id,
    intake_pump_override,
    collect_pump_override,
    mist_override,
    sleep,
  } = req.body;

  if (!device_id) return res.status(400).json({ error: 'device_id required' });

  const { error } = await supabase
    .from('device_commands')
    .upsert({
      device_id,
      intake_pump_override:  intake_pump_override  ?? 'auto',
      collect_pump_override: collect_pump_override ?? 'auto',
      mist_override:         mist_override         ?? 'auto',
      sleep:                 sleep                 ?? false,
      updated_at:            new Date().toISOString(),
    });

  if (error) return res.status(500).json({ error });
  return res.status(200).json({ ok: true });
}
```

### 2.4 Latest Reading Endpoint — `pages/api/readings/latest.ts`

For the mobile app to fetch current sensor data.

```ts
import { createClient } from '@supabase/supabase-js';
import type { NextApiRequest, NextApiResponse } from 'next';

const supabase = createClient(
  process.env.SUPABASE_URL!,
  process.env.SUPABASE_SERVICE_KEY!
);

export default async function handler(req: NextApiRequest, res: NextApiResponse) {
  if (req.method !== 'GET') return res.status(405).end();

  const { data, error } = await supabase
    .from('sensor_readings')
    .select('*')
    .order('created_at', { ascending: false })
    .limit(1)
    .single();

  if (error) return res.status(500).json({ error });
  return res.status(200).json(data);
}
```

---

## 3. Expo Mobile App Setup

### 3.1 Install Dependencies

```bash
npx expo install @tanstack/react-query
```

Or with npm:
```bash
npm install @tanstack/react-query
```

### 3.2 API Config — `constants/api.ts`

```ts
export const API_BASE    = 'https://walrus-pi.vercel.app/api';
export const DEVICE_ID   = 'WALRUS_001';
```

### 3.3 Dashboard Screen — `app/index.tsx`

Displays live sensor readings, auto-refreshes every 15 seconds.

```tsx
import React, { useEffect, useState } from 'react';
import { View, Text, StyleSheet, ActivityIndicator, ScrollView, RefreshControl } from 'react-native';
import { API_BASE } from '../constants/api';

export default function DashboardScreen() {
  const [data,       setData]       = useState<any>(null);
  const [loading,    setLoading]    = useState(true);
  const [refreshing, setRefreshing] = useState(false);

  const fetchData = async () => {
    try {
      const res = await fetch(`${API_BASE}/readings/latest`);
      const json = await res.json();
      setData(json);
    } catch (e) {
      console.error(e);
    } finally {
      setLoading(false);
      setRefreshing(false);
    }
  };

  useEffect(() => {
    fetchData();
    const interval = setInterval(fetchData, 15000);
    return () => clearInterval(interval);
  }, []);

  if (loading) return <ActivityIndicator style={{ flex: 1 }} />;

  return (
    <ScrollView
      style={styles.container}
      refreshControl={<RefreshControl refreshing={refreshing} onRefresh={() => { setRefreshing(true); fetchData(); }} />}
    >
      <Text style={styles.title}>Walrus Dashboard</Text>

      <View style={styles.card}>
        <Text style={styles.cardTitle}>Sensors</Text>
        <Row label="Temperature"   value={`${data?.basin_temp ?? '--'} °C`} />
        <Row label="TDS"           value={`${data?.tds_ppm ?? '--'} ppm`} />
        <Row label="Clean Water"   value={`${data?.clean_level_cm ?? '--'} cm`} />
      </View>

      <View style={styles.card}>
        <Text style={styles.cardTitle}>Actuators</Text>
        <Row label="Intake Pump"     value={data?.intake_pump_active  ? 'ON' : 'OFF'} />
        <Row label="Collection Pump" value={data?.collect_pump_active ? 'ON' : 'OFF'} />
        <Row label="Mist/Atomizer"   value={data?.mist_active         ? 'ON' : 'OFF'} />
        <Row label="Float Switch"    value={data?.float_water_detect  ? 'Water Detected' : 'No Water'} />
      </View>

      <View style={styles.card}>
        <Text style={styles.cardTitle}>System</Text>
        <Row label="State"      value={data?.state ?? '--'} />
        <Row label="Last Sync"  value={data?.created_at ? new Date(data.created_at).toLocaleTimeString() : '--'} />
      </View>
    </ScrollView>
  );
}

const Row = ({ label, value }: { label: string; value: string }) => (
  <View style={styles.row}>
    <Text style={styles.rowLabel}>{label}</Text>
    <Text style={styles.rowValue}>{value}</Text>
  </View>
);

const styles = StyleSheet.create({
  container:  { flex: 1, backgroundColor: '#f5f5f5', padding: 16 },
  title:      { fontSize: 24, fontWeight: 'bold', marginBottom: 16 },
  card:       { backgroundColor: '#fff', borderRadius: 12, padding: 16, marginBottom: 16 },
  cardTitle:  { fontSize: 16, fontWeight: '600', marginBottom: 12, color: '#555' },
  row:        { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 6, borderBottomWidth: 1, borderBottomColor: '#f0f0f0' },
  rowLabel:   { fontSize: 15, color: '#333' },
  rowValue:   { fontSize: 15, fontWeight: '500', color: '#2563eb' },
});
```

### 3.4 Controls Screen — `app/controls.tsx`

Manual override controls for pumps, mist, and sleep.

```tsx
import React, { useState } from 'react';
import { View, Text, Switch, StyleSheet, TouchableOpacity, Alert, ScrollView } from 'react-native';
import { API_BASE, DEVICE_ID } from '../constants/api';

type Override = 'auto' | 'on' | 'off';

export default function ControlsScreen() {
  const [intakePump,  setIntakePump]  = useState<Override>('auto');
  const [collectPump, setCollectPump] = useState<Override>('auto');
  const [mist,        setMist]        = useState<Override>('auto');
  const [sleep,       setSleep]       = useState(false);
  const [sending,     setSending]     = useState(false);

  const sendCommands = async () => {
    setSending(true);
    try {
      const res = await fetch(`${API_BASE}/commands`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          device_id:             DEVICE_ID,
          intake_pump_override:  intakePump,
          collect_pump_override: collectPump,
          mist_override:         mist,
          sleep,
        }),
      });
      if (res.ok) Alert.alert('Done', 'Commands sent. Device will update within 15 seconds.');
      else        Alert.alert('Error', 'Failed to send commands.');
    } catch {
      Alert.alert('Error', 'Network error.');
    } finally {
      setSending(false);
    }
  };

  const resetAll = () => {
    setIntakePump('auto');
    setCollectPump('auto');
    setMist('auto');
    setSleep(false);
  };

  return (
    <ScrollView style={styles.container}>
      <Text style={styles.title}>Manual Controls</Text>
      <Text style={styles.subtitle}>Commands apply within 15 seconds</Text>

      <OverrideControl label="Intake Pump"     value={intakePump}  onChange={setIntakePump}  />
      <OverrideControl label="Collection Pump" value={collectPump} onChange={setCollectPump} />
      <OverrideControl label="Mist / Atomizer" value={mist}        onChange={setMist}        />

      <View style={styles.card}>
        <View style={styles.switchRow}>
          <View>
            <Text style={styles.switchLabel}>Sleep Device</Text>
            <Text style={styles.switchSub}>ESP32 will sleep until 6 AM</Text>
          </View>
          <Switch value={sleep} onValueChange={setSleep} />
        </View>
      </View>

      <TouchableOpacity style={styles.resetBtn} onPress={resetAll}>
        <Text style={styles.resetText}>Reset All to Auto</Text>
      </TouchableOpacity>

      <TouchableOpacity
        style={[styles.sendBtn, sending && styles.sendBtnDisabled]}
        onPress={sendCommands}
        disabled={sending}
      >
        <Text style={styles.sendText}>{sending ? 'Sending...' : 'Send Commands'}</Text>
      </TouchableOpacity>
    </ScrollView>
  );
}

const OverrideControl = ({
  label, value, onChange
}: { label: string; value: Override; onChange: (v: Override) => void }) => (
  <View style={styles.card}>
    <Text style={styles.cardLabel}>{label}</Text>
    <View style={styles.btnGroup}>
      {(['auto', 'on', 'off'] as Override[]).map(opt => (
        <TouchableOpacity
          key={opt}
          style={[styles.optBtn, value === opt && styles.optBtnActive]}
          onPress={() => onChange(opt)}
        >
          <Text style={[styles.optText, value === opt && styles.optTextActive]}>
            {opt.toUpperCase()}
          </Text>
        </TouchableOpacity>
      ))}
    </View>
  </View>
);

const styles = StyleSheet.create({
  container:       { flex: 1, backgroundColor: '#f5f5f5', padding: 16 },
  title:           { fontSize: 24, fontWeight: 'bold', marginBottom: 4 },
  subtitle:        { fontSize: 13, color: '#888', marginBottom: 20 },
  card:            { backgroundColor: '#fff', borderRadius: 12, padding: 16, marginBottom: 12 },
  cardLabel:       { fontSize: 16, fontWeight: '500', marginBottom: 12 },
  btnGroup:        { flexDirection: 'row', gap: 8 },
  optBtn:          { flex: 1, paddingVertical: 10, borderRadius: 8, borderWidth: 1, borderColor: '#ddd', alignItems: 'center' },
  optBtnActive:    { backgroundColor: '#2563eb', borderColor: '#2563eb' },
  optText:         { color: '#555', fontWeight: '500' },
  optTextActive:   { color: '#fff', fontWeight: '600' },
  switchRow:       { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  switchLabel:     { fontSize: 16, fontWeight: '500' },
  switchSub:       { fontSize: 12, color: '#888', marginTop: 2 },
  resetBtn:        { borderWidth: 1, borderColor: '#ddd', borderRadius: 10, padding: 14, alignItems: 'center', marginBottom: 12 },
  resetText:       { color: '#555', fontSize: 15 },
  sendBtn:         { backgroundColor: '#2563eb', borderRadius: 10, padding: 16, alignItems: 'center', marginBottom: 32 },
  sendBtnDisabled: { backgroundColor: '#93c5fd' },
  sendText:        { color: '#fff', fontSize: 16, fontWeight: 'bold' },
});
```

### 3.5 Navigation — `app/_layout.tsx`

```tsx
import { Tabs } from 'expo-router';
import { Ionicons } from '@expo/vector-icons';

export default function Layout() {
  return (
    <Tabs>
      <Tabs.Screen
        name="index"
        options={{
          title: 'Dashboard',
          tabBarIcon: ({ color }) => <Ionicons name="water" size={24} color={color} />,
        }}
      />
      <Tabs.Screen
        name="controls"
        options={{
          title: 'Controls',
          tabBarIcon: ({ color }) => <Ionicons name="settings" size={24} color={color} />,
        }}
      />
    </Tabs>
  );
}
```

---

## 4. Deployment Checklist

### Supabase
- [ ] `sensor_readings` table created
- [ ] `device_commands` table created
- [ ] Default row inserted for `WALRUS_001`

### Vercel
- [ ] `SUPABASE_URL` env var set
- [ ] `SUPABASE_SERVICE_KEY` env var set
- [ ] `ESP32_API_KEY` env var set
- [ ] `/api/esp32/data` endpoint deployed
- [ ] `/api/commands` endpoint deployed
- [ ] `/api/readings/latest` endpoint deployed

### Expo App
- [ ] `constants/api.ts` pointing to correct Vercel URL
- [ ] Dashboard screen showing live data
- [ ] Controls screen sending commands successfully
- [ ] Navigation between Dashboard and Controls working

### ESP32
- [ ] Uploads data every 15 seconds
- [ ] Reads commands from API response
- [ ] Serial monitor shows `[CMD] Overrides` line on each sync
- [ ] Deep sleep triggers at 8 PM, wakes at 6 AM

---

## 5. Testing the Override Flow

1. Open Expo app → Controls screen
2. Set **Intake Pump** to `ON`
3. Tap **Send Commands**
4. Wait up to 15 seconds
5. Check ESP32 serial monitor for:
   ```
   [CMD] Overrides — IN:on COL:auto MIST:auto
   ```
6. Intake pump should activate regardless of float switch state
7. Set back to `AUTO` when done testing