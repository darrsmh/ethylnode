import { createSupabaseServer } from "./supabase-server";

const SAMPLE_COLUMNS =
  "node_id, ts, pga_c, sigma_f, sigma_a, sigma_m, snr_db, roll, pitch";

function sb() {
  return createSupabaseServer();
}

// ── Live state ────────────────────────────────────────────────

export async function updateLive(data: Record<string, unknown>) {
  const { node_id, ...fields } = data;
  await sb()
    .from("live_state")
    .upsert(
      { node_id: node_id ?? "ADXL345-01", ...fields, updated_at: new Date().toISOString() },
      { onConflict: "node_id" }
    );
}

export async function getLive() {
  const { data } = await sb()
    .from("live_state")
    .select("*")
    .eq("node_id", "ADXL345-01")
    .single();
  return data ?? {};
}

// ── Samples ───────────────────────────────────────────────────

export async function pushSamples(samples: object[]) {
  if (!samples.length) return;
  const rows = samples.map((s) => {
    const r = s as Record<string, unknown>;
    return {
      node_id: "ADXL345-01",
      ts: r.ts,
      pga_c: r.pga_c,
      sigma_f: r.sigma_f,
      sigma_a: r.sigma_a,
      sigma_m: r.sigma_m,
      snr_db: r.snr_db,
      roll: r.roll,
      pitch: r.pitch,
    };
  });
  await sb().from("samples").insert(rows);
}

export async function getSamples(count = 200) {
  const { data } = await sb()
    .from("samples")
    .select(SAMPLE_COLUMNS)
    .order("ts", { ascending: false })
    .limit(Math.min(count, 600));
  return (data ?? []).reverse();
}

// ── Alerts ────────────────────────────────────────────────────

export async function pushAlert(alert: object) {
  const r = alert as Record<string, unknown>;
  await sb().from("alerts").insert({
    node_id: r.node_id,
    event_type: r.event_type,
    pga: r.pga,
    pga_mgal: r.pga_mgal,
    ts_ms: r.ts_ms,
    snr_db: r.snr_db,
    sigma_fused: r.sigma_fused,
    sigma_adxl: r.sigma_adxl,
    sigma_mpu: r.sigma_mpu,
    ax_corr: r.ax_corr,
    ay_corr: r.ay_corr,
    az_corr: r.az_corr,
    noise_reduction_eta: r.noise_reduction_eta,
    noise_reduction_pct: r.noise_reduction_pct,
  });
}

export async function getAlerts(count = 20) {
  const { data } = await sb()
    .from("alerts")
    .select("*")
    .order("ts_ms", { ascending: false })
    .limit(count);
  return data ?? [];
}

// ── Heartbeats ────────────────────────────────────────────────

export async function setHeartbeat(nodeId: string, data: Record<string, unknown>) {
  await sb()
    .from("heartbeats")
    .upsert(
      { node_id: nodeId, ...data, updated_at: new Date().toISOString() },
      { onConflict: "node_id" }
    );
}

export async function getHeartbeat(nodeId: string) {
  const { data } = await sb()
    .from("heartbeats")
    .select("*")
    .eq("node_id", nodeId)
    .single();
  return data ?? {};
}
