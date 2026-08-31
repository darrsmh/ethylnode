"use client";

import { useEffect, useState, useCallback } from "react";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  Tooltip,
  ResponsiveContainer,
  CartesianGrid,
  Legend,
} from "recharts";

interface Live {
  node_id?: string;
  ts?: number;
  pga_c?: number;
  sigma_f?: number;
  sigma_a?: number;
  sigma_m?: number;
  snr_db?: number;
  roll?: number;
  pitch?: number;
  last_alert_pga?: number;
  last_alert_ts?: number;
}

interface Sample {
  ts: number;
  pga_c: number;
  sigma_f: number;
  sigma_a: number;
  sigma_m: number;
  snr_db: number;
  roll: number;
  pitch: number;
}

interface Alert {
  pga: number;
  ts_ms: number;
  snr_db: number;
  sigma_fused: number;
}

function fmt(n: number | undefined, d = 5) {
  return n !== undefined ? n.toFixed(d) : "--";
}

function ago(ms: number) {
  const s = Math.floor((Date.now() - ms) / 1000);
  if (s < 60) return `${s}s ago`;
  if (s < 3600) return `${Math.floor(s / 60)}m ago`;
  return `${Math.floor(s / 3600)}h ago`;
}

export default function Dashboard() {
  const [live, setLive] = useState<Live>({});
  const [history, setHistory] = useState<Sample[]>([]);
  const [alerts, setAlerts] = useState<Alert[]>([]);
  const [online, setOnline] = useState(false);

  const fetchLive = useCallback(async () => {
    try {
      const res = await fetch("/api/live", { cache: "no-store" });
      const data = await res.json();
      setLive(data);
      setOnline(!!data.ts && Date.now() - Number(data.ts) < 30000);
    } catch {
      setOnline(false);
    }
  }, []);

  const fetchHistory = useCallback(async () => {
    try {
      const res = await fetch("/api/live/history?count=200", { cache: "no-store" });
      const data = await res.json();
      setHistory(data);
    } catch {}
  }, []);

  useEffect(() => {
    fetchLive();
    fetchHistory();
    const t1 = setInterval(fetchLive, 1000);
    const t2 = setInterval(fetchHistory, 5000);
    return () => { clearInterval(t1); clearInterval(t2); };
  }, [fetchLive, fetchHistory]);

  const chartData = history.map((s) => ({
    ...s,
    time: new Date(s.ts).toLocaleTimeString(),
  }));

  return (
    <div className="dashboard">
      <h1>
        Seismic Monitor
        <span className={online ? "online-badge" : "offline-badge"}>
          {online ? "LIVE" : "OFFLINE"}
        </span>
      </h1>
      <div className="subtitle">
        Node {live.node_id ?? "--"} | Last update {live.ts ? ago(Number(live.ts)) : "never"}
      </div>

      <div className="status-row">
        <div className="status-card">
          <div className="label">PGA</div>
          <div className="value">
            {fmt(live.pga_c, 5)}
            <span className="unit">g</span>
          </div>
        </div>
        <div className="status-card">
          <div className="label">Sigma (fused)</div>
          <div className="value">
            {fmt(live.sigma_f, 6)}
            <span className="unit">g</span>
          </div>
        </div>
        <div className="status-card">
          <div className="label">Sigma (ADXL)</div>
          <div className="value">
            {fmt(live.sigma_a, 6)}
            <span className="unit">g</span>
          </div>
        </div>
        <div className="status-card">
          <div className="label">Sigma (MPU)</div>
          <div className="value">
            {fmt(live.sigma_m, 6)}
            <span className="unit">g</span>
          </div>
        </div>
        <div className={`status-card ${(live.snr_db ?? 0) > 20 ? "ok" : ""}`}>
          <div className="label">SNR</div>
          <div className="value">
            {fmt(live.snr_db, 1)}
            <span className="unit">dB</span>
          </div>
        </div>
        <div className="status-card">
          <div className="label">Roll</div>
          <div className="value">
            {fmt(live.roll, 2)}
            <span className="unit">deg</span>
          </div>
        </div>
        <div className="status-card">
          <div className="label">Pitch</div>
          <div className="value">
            {fmt(live.pitch, 2)}
            <span className="unit">deg</span>
          </div>
        </div>
      </div>

      <div className="chart-section">
        <h2>PGA History</h2>
        {chartData.length > 0 ? (
          <ResponsiveContainer width="100%" height={300}>
            <LineChart data={chartData}>
              <CartesianGrid strokeDasharray="3 3" stroke="#222" />
              <XAxis dataKey="time" tick={{ fontSize: 11, fill: "#666" }} />
              <YAxis tick={{ fontSize: 11, fill: "#666" }} />
              <Tooltip
                contentStyle={{ background: "#1a1a2a", border: "1px solid #333", borderRadius: 4 }}
                labelStyle={{ color: "#888" }}
              />
              <Legend wrapperStyle={{ fontSize: 12 }} />
              <Line type="monotone" dataKey="pga_c" stroke="#ef4444" dot={false} name="PGA" />
              <Line type="monotone" dataKey="sigma_f" stroke="#3b82f6" dot={false} name="Sigma (fused)" />
              <Line type="monotone" dataKey="sigma_a" stroke="#22c55e" dot={false} name="Sigma (ADXL)" />
              <Line type="monotone" dataKey="sigma_m" stroke="#f59e0b" dot={false} name="Sigma (MPU)" />
            </LineChart>
          </ResponsiveContainer>
        ) : (
          <div className="no-data">Waiting for data...</div>
        )}
      </div>

      <div className="chart-section">
        <h2>Roll / Pitch</h2>
        {chartData.length > 0 ? (
          <ResponsiveContainer width="100%" height={200}>
            <LineChart data={chartData}>
              <CartesianGrid strokeDasharray="3 3" stroke="#222" />
              <XAxis dataKey="time" tick={{ fontSize: 11, fill: "#666" }} />
              <YAxis tick={{ fontSize: 11, fill: "#666" }} />
              <Tooltip
                contentStyle={{ background: "#1a1a2a", border: "1px solid #333", borderRadius: 4 }}
                labelStyle={{ color: "#888" }}
              />
              <Legend wrapperStyle={{ fontSize: 12 }} />
              <Line type="monotone" dataKey="roll" stroke="#a855f7" dot={false} name="Roll" />
              <Line type="monotone" dataKey="pitch" stroke="#06b6d4" dot={false} name="Pitch" />
            </LineChart>
          </ResponsiveContainer>
        ) : (
          <div className="no-data">Waiting for data...</div>
        )}
      </div>

      <div className="alerts-list">
        <h2>Recent Alerts</h2>
        {alerts.length === 0 && <div className="no-data">No alerts yet</div>}
        {alerts.map((a, i) => (
          <div className="alert-item" key={i}>
            <span>
              PGA: <span className="pga">{a.pga?.toFixed(5)}g</span>
              {" "} | SNR: {a.snr_db?.toFixed(1)} dB
            </span>
            <span className="time">{a.ts_ms ? ago(a.ts_ms) : ""}</span>
          </div>
        ))}
      </div>
    </div>
  );
}
