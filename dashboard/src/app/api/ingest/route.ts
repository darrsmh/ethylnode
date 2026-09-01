// Edge Runtime — runs on V8 isolates at Vercel's nearest PoP to the ESP32.
// No cold start. No Node.js boot overhead. Always warm.
export const runtime = "edge";

import { NextRequest, NextResponse } from "next/server";
import { updateLive, pushSamples } from "@/lib/db";

function verifyKey(req: NextRequest) {
  return req.headers.get("x-api-key") === process.env.API_KEY;
}

// Module-level throttle: persists within a warm isolate.
// Each edge region has its own isolate so updateLive may fire slightly more
// than once/sec across regions — harmless (it's just an upsert).
let lastLiveUpdate = 0;

export async function POST(req: NextRequest) {
  if (!verifyKey(req))
    return NextResponse.json({ error: "unauthorized" }, { status: 401 });

  const body = await req.json();
  const { node_id, samples } = body;

  if (!samples?.length)
    return NextResponse.json({ ok: true, stored: 0 });

  // Run pushSamples and updateLive concurrently — they don't depend on each other.
  const now = Date.now();
  const shouldUpdateLive = now - lastLiveUpdate > 1000;
  if (shouldUpdateLive) lastLiveUpdate = now;

  const tasks: Promise<unknown>[] = [pushSamples(samples)];

  if (shouldUpdateLive) {
    const latest = samples[samples.length - 1];
    tasks.push(
      updateLive({
        node_id,
        ts:      latest.ts,
        pga_c:   latest.pga_c,
        sigma_f: latest.sigma_f,
        sigma_a: latest.sigma_a,
        sigma_m: latest.sigma_m,
        snr_db:  latest.snr_db,
        roll:    latest.roll,
        pitch:   latest.pitch,
      })
    );
  }

  await Promise.all(tasks);
  return NextResponse.json({ ok: true, stored: samples.length });
}
