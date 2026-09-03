export const runtime = "edge";

import { NextRequest, NextResponse } from "next/server";
import { getSamples } from "@/lib/db";

export async function GET(req: NextRequest) {
  const count = parseInt(req.nextUrl.searchParams.get("count") ?? "200", 10);
  const samples = await getSamples(Math.min(count, 6000));
  return NextResponse.json(samples);
}
