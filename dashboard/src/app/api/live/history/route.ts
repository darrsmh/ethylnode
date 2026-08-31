import { NextRequest, NextResponse } from "next/server";
import { getSamples } from "@/lib/redis";

export const dynamic = "force-dynamic";

export async function GET(req: NextRequest) {
  const count = parseInt(req.nextUrl.searchParams.get("count") ?? "200", 10);
  const samples = await getSamples(Math.min(count, 600));
  return NextResponse.json(samples);
}
