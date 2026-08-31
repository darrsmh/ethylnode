import { NextRequest, NextResponse } from "next/server";
import { pushAlert, updateLive } from "@/lib/redis";

function verifyKey(req: NextRequest) {
  return req.headers.get("x-api-key") === process.env.API_KEY;
}

export async function POST(req: NextRequest) {
  if (!verifyKey(req)) return NextResponse.json({ error: "unauthorized" }, { status: 401 });

  const body = await req.json();
  await pushAlert(body);

  await updateLive({
    last_alert_pga: body.pga,
    last_alert_ts: body.ts_ms,
    last_alert_snr: body.snr_db,
  });

  return NextResponse.json({ ok: true });
}
