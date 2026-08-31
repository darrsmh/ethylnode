import { NextRequest, NextResponse } from "next/server";
import { redis, updateLive, pushSamples } from "@/lib/redis";

function verifyKey(req: NextRequest) {
  const key = req.headers.get("x-api-key");
  if (key !== process.env.API_KEY) return false;
  return true;
}

export async function POST(req: NextRequest) {
  if (!verifyKey(req)) return NextResponse.json({ error: "unauthorized" }, { status: 401 });

  const body = await req.json();
  const { node_id, samples } = body;

  if (samples?.length) {
    await pushSamples(samples);

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

  return NextResponse.json({ ok: true, stored: samples?.length ?? 0 });
}
