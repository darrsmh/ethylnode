import { NextRequest, NextResponse } from "next/server";
import { setHeartbeat } from "@/lib/db";

function verifyKey(req: NextRequest) {
  return req.headers.get("x-api-key") === process.env.API_KEY;
}

export async function POST(req: NextRequest) {
  if (!verifyKey(req)) return NextResponse.json({ error: "unauthorized" }, { status: 401 });

  const body = await req.json();
  await setHeartbeat(body.node_id, {
    ts: body.ts_ms,
    status: body.status,
    rssi: body.rssi_dbm,
    heap: body.free_heap,
  });

  return NextResponse.json({ ok: true });
}
