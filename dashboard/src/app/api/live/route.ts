import { NextResponse } from "next/server";
import { getLive } from "@/lib/db";

export const dynamic = "force-dynamic";

export async function GET() {
  const live = await getLive();
  return NextResponse.json(live || {});
}
