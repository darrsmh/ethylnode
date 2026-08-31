import { NextRequest, NextResponse } from "next/server";
import { updateLive, pushSamples } from "@/lib/db";

function verifyKey(req: NextRequest) {
  return req.headers.get("x-api-key") === process.env.API_KEY;
}

// Throttle live_state upserts to ~1/sec even though the ESP32 now batches
// every 100ms. The dashboard's Realtime push already updates live state
// client-side on every sample insert, so this server-side upsert only needs
// to keep the persistent row fresh for cold loads.
let lastLiveUpdate = 0;

export async function POST(req: NextRequest) {
  if (!verifyKey(req)) return NextResponse.json({ error: "unauthorized" }, { status: 401 });

  const body = await req.json();
  const { node_id, samples } = body;

  if (samples?.length) {
    await pushSamples(samples);

    const now = Date.now();
    if (now - lastLiveUpdate > 1000) {
      lastLiveUpdate = now;
      const latest = samples[samples.length - 1];
      await updateLive({
        node_id,
        ts: latest.ts,
        pga_c: latest.pga_c,
        sigma_f: latest.sigma_f,
        sigma_a: latest.sigma_a,
        sigma_m: latest.sigma_m,
        snr_db: latest.snr_db,
        roll: latest.roll,
        pitch: latest.pitch,
      });
    }
  }

  return NextResponse.json({ ok: true, stored: samples?.length ?? 0 });
}
